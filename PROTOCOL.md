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

## v2 datagram

Every datagram is a 24-byte header plus payload. All fields big-endian (the Wii U is a
big-endian PowerPC; this is also Java's `ByteBuffer` default, so neither side byte-swaps).

```
 offset  size  field
      0     4  magic       0x57555332  ('W','U','S','2')
      4     4  frameId     increments by 1 per frame, wraps freely
      8     4  frameSize   total JPEG size in bytes
     12     4  chunkOffset byte offset of this chunk inside the frame
     16     2  chunkLen    payload bytes in this datagram
     18     1  flags       bit0 = last chunk of the frame
     19     1  version     2
     20     4  frameCrc    CRC-32 of the whole JPEG, repeated in every chunk
     24  chunkLen  payload
```

`STREAM_MAX_PAYLOAD` is 1376, so a full datagram is 1400 bytes on the wire — inside the
1472-byte ceiling for a 1500-byte-MTU LAN, so IP never fragments it.

`frameCrc` is repeated in every chunk on purpose: it costs 4 bytes and it means a client
that joins mid-frame, or that lost the first chunk, can still validate whatever it does
manage to assemble instead of having to guess.

## Receiver rules

* Drop anything whose `magic` or `version` doesn't match, or whose actual datagram length
  isn't `24 + chunkLen`.
* Sanity-check `frameSize` against a hard cap (`MAX_FRAME_BYTES`, 8 MB) and
  `chunkOffset + chunkLen <= frameSize` **before** allocating or copying anything.
* Assemble into a buffer keyed by `frameId`. A datagram for a newer `frameId` retires the
  frame in progress — incomplete frames are discarded, never rendered.
* **Clear the staleness filter on every (re)connect.** `frameId` is monotonic only within one
  run of the plugin: the console keeps counting across a client reconnect, but a title change
  reloads the plugin and restarts it at 0. A receiver that rejects "not newer than the last
  frame I completed" without resetting on connect will silently discard an entire new session
  until the counter climbs past the old one — minutes of black window.
* Count received bytes with a per-chunk seen-set so a duplicated datagram can't make an
  incomplete frame look finished.
* When the received byte count reaches `frameSize`, verify CRC-32 and decode. On mismatch,
  drop the frame and carry on — the next frame is unaffected.

## CRC-32

Standard CRC-32 (reflected, polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR
`0xFFFFFFFF`) — bit-identical to `java.util.zip.CRC32`, so the client uses the JDK
implementation directly.

## Mixing versions

A v1 client pointed at a v2 plugin sees no valid frames (its first datagram is 1400 bytes,
not 4, so it stays in `UNKNOWN` forever) and shows a black window. A v2 client pointed at a
v1 plugin logs `ignoring N non-v2 datagrams - is the Wii U running the old plugin?` once a
second. Use the matching pair.
