# Wire protocol

Two sockets, same as upstream:

| Socket | Port | Direction | Purpose |
| --- | --- | --- | --- |
| TCP | 8092 | client -> Wii U | control / heartbeat. Wii U listens. |
| UDP | 9445 | Wii U -> client | video. Client listens. |

The client opens the TCP connection; the Wii U learns the client's IP from the accepted
socket and starts blasting UDP at `<client ip>:9445`. The client sends `0x15` about once a
second and the Wii U answers `0x16`. If the heartbeat stops, the Wii U tears the video
socket down.

## Why the protocol changed

Upstream sent each frame as three *unlabelled* bursts of datagrams:

```
[4 bytes  CRC32 ] [8 bytes  length ] [ length bytes of JPEG, in <=1400 byte datagrams ]
```

The receiver was a state machine that inferred meaning purely from datagram *size* and
*arrival order*. That has three fatal properties, all of which get much worse at 720p
because a frame goes from ~25 datagrams to ~100:

1. **A single lost datagram corrupts the next frame too.** The receiver keeps counting
   down `remaining`, so it swallows the following frame's 4-byte CRC and 8-byte length as
   if they were pixel data, then fails the CRC on *both* frames.
2. **A trailing chunk of exactly 4 or 8 bytes is indistinguishable from a header.**
   `size % 1400 == 4` happens roughly 1 frame in 350.
3. **A desync can allocate an arbitrary buffer.** `curJPEGSize = (int) wrapped.getLong()`
   followed by `new byte[curJPEGSize]` throws `NegativeArraySizeException` or
   `OutOfMemoryError` and kills the receive thread for good.

So v2 makes every datagram self-describing. Loss is still loss — but it is now *contained*
to the frame that lost a packet, it is detected immediately, and it can never desync the
stream or crash the client.

## v3 datagram

Every datagram is a 44-byte header plus payload. All fields big-endian (the Wii U is a
big-endian PowerPC; this is also Java's `ByteBuffer` default, so neither side byte-swaps).

v3 makes each datagram fully self-describing: it carries a timestamp, the frame's
width/height/stride, the compression type and the pixel format, so the client renders
whatever a frame declares itself to be rather than assuming JPEG at a fixed size. That is
what lets one wire format carry JPEG today and RAW or a lightweight codec later without the
client being told in advance which to expect.

```
 offset  size  field
      0     4  magic           0x57555333  ('W','U','S','3')
      4     4  frameId         increments by 1 per frame, wraps freely
      8     8  timestampUs     console clock in microseconds, frame-constant
     16     4  frameSize       total payload size in bytes
     20     4  chunkOffset     byte offset of this chunk inside the frame
     24     4  frameCrc        CRC-32 of the whole payload, repeated in every chunk
     28     2  width           frame width in pixels
     30     2  height          frame height in pixels
     32     4  stride          bytes per row for RAW, 0 for JPEG
     36     2  chunkLen        payload bytes in this datagram
     38     1  flags           bit0 = last chunk, bit1 = keyframe
     39     1  version         3
     40     1  compressionType 0 = RAW, 1 = LIGHTWEIGHT, 2 = JPEG
     41     1  pixelFormat     0 = JPEG, 1 = RGB888, 2 = RGBA8888
     42     2  reserved        0
     44  chunkLen  payload
```

`STREAM_MAX_PAYLOAD` is 1376, so a full datagram is 1420 bytes on the wire — inside the
1472-byte ceiling for a 1500-byte-MTU LAN, so IP never fragments it. The payload size and
chunk stride are unchanged from v2, so the reassembly and per-chunk seen-set logic did not
change — only the header grew.

The whole-frame fields (`frameSize`, `frameCrc`, `timestampUs`, `width`, `height`,
`stride`, `compressionType`, `pixelFormat`) are repeated identically in every chunk, so a
client that joined mid-frame or lost the first chunk can still validate and decode whatever
it does manage to assemble instead of having to guess.

`timestampUs` is the console's own clock. It is for ordering and jitter measurement only —
the two clocks are not synchronised, so a difference against the PC clock is **not** a
latency measurement.

## Receiver rules

* Drop anything whose `magic` or `version` doesn't match, or whose actual datagram length
  isn't `44 + chunkLen`.
* Sanity-check `frameSize` against a hard cap (`MAX_FRAME_BYTES`, 8 MB) and
  `chunkOffset + chunkLen <= frameSize` **before** allocating or copying anything.
* For a `RAW` frame, require `stride >= width * bytesPerPixel` and
  `height * stride == frameSize` before trusting the geometry — otherwise a spoofed
  width/height is an out-of-bounds read waiting to happen. JPEG carries its own dimensions,
  so its header `width`/`height`/`stride` are advisory.
* Assemble into a buffer keyed by `frameId`, and **keep the previous frame open while the next
  one starts**: the network can deliver the last chunk of frame N just after the first chunk of
  frame N+1 (measured on hardware: about three of every four "incomplete" frames were this).
  Frame N may still be shown if it completes before N+1 does; once N+1 is shown, or N+2
  starts, N is discarded. Frames are never rendered out of order, and incomplete frames are
  never rendered.
* **Clear the staleness filter on every (re)connect.** `frameId` is monotonic only within one
  run of the plugin: the console keeps counting across a client reconnect, but a title change
  reloads the plugin and restarts it at 0. A receiver that rejects "not newer than the last
  frame I completed" without resetting on connect will silently discard an entire new session
  until the counter climbs past the old one — minutes of black window.
* Count received bytes with a per-chunk seen-set so a duplicated datagram can't make an
  incomplete frame look finished.
* When the received byte count reaches `frameSize`, verify CRC-32 and decode. On mismatch,
  drop the frame and carry on — the next frame is unaffected.

## Side channel: audio and status

Two more datagram types share the video's UDP port. They are told apart by their first four
bytes; a receiver that does not know a magic ignores the datagram. The console sends them
from a second socket, so they never wait behind a video frame.

### Audio — `WUSA`, version 1

| Offset | Size | Field        | Meaning                                                        |
|-------:|-----:|--------------|----------------------------------------------------------------|
| 0      | 4    | `magic`      | `0x57555341` ("WUSA")                                          |
| 4      | 1    | `version`    | 1                                                              |
| 5      | 1    | `codec`      | 1 = IMA ADPCM, 4 bits per sample                               |
| 6      | 1    | `channels`   | 2                                                              |
| 7      | 1    | reserved     | 0                                                              |
| 8      | 4    | `sampleRate` | Hz (48000, or 32000 for titles using the 32 kHz renderer)      |
| 12     | 4    | `sequence`   | +1 per block, wraps                                            |
| 16     | 4    | `firstFrame` | sample-frame index of the block's first frame, wraps           |
| 20     | 2    | `frames`     | sample frames in the block (960 = 20 ms at 48 kHz)             |
| 22     | 2    | reserved     | 0                                                              |
| 24     | 4    | `predictor`  | int16 left, int16 right: ADPCM state at the first frame        |
| 28     | 2    | `stepIndex`  | uint8 left, uint8 right                                        |
| 30     | 2    | reserved     | 0                                                              |
| 32     | n    | data         | one byte per stereo frame: left code in the low nibble         |

Each block carries the decoder state it starts from, so it decodes on its own — a lost block
costs its own 20 ms and nothing after it. `firstFrame` lets a receiver fill a gap with exactly
the silence it stands for, keeping sound and picture in step. The codec is standard IMA/DVI
ADPCM (89-entry step table, index table `-1 -1 -1 -1 2 4 6 8`).

### Status — `WUSS`, version 1

| Offset | Size | Field     | Meaning                  |
|-------:|-----:|-----------|--------------------------|
| 0      | 4    | `magic`   | `0x57555353` ("WUSS")    |
| 4      | 1    | `version` | 1                        |
| 5      | 1    | `kind`    | 0 = text                 |
| 6      | 2    | `length`  | bytes of text that follow (at most 1200) |

Plain ASCII, about once a second, one item per line: `settings ...` (the plugin menu),
`state ...` (what is running — game, Wii U Menu — and what is being streamed) and `perf ...`
(the console's own frame rates and bottleneck counters). The format of each line is for
people, not parsers; the client logs them.

## CRC-32

Standard CRC-32 (reflected, polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR
`0xFFFFFFFF`) — bit-identical to `java.util.zip.CRC32`, so the client uses the JDK
implementation directly.

## Mixing versions

The magic and version are bumped together (`WUS2`/2 → `WUS3`/3), so a mismatched pair simply
rejects every datagram rather than misparsing one. A client built for a different protocol
version sees no valid frames and shows a black window while `ignored=` climbs in its stats
line. Always run the plugin and client from the same build.
