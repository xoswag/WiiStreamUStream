# StreamingPluginClient — 720p fork

PC client for the [720p fork of the Wii U ScreenStreaming plugin](https://github.com/Maschell/StreamingPluginWiiU).

> This client speaks **protocol v2** and only v2. Point it at a console running the original
> plugin and you get a black window; the log will say so once a second. See `PROTOCOL.md` in the
> plugin repository for the format and why it changed.

## Running

Needs **Java 11 or newer**.

```bash
java -jar StreamingPluginClient-0.2.0-app.jar
```

It asks for the console's IP address. To skip the prompt:

```bash
java -jar StreamingPluginClient-0.2.0-app.jar -ip 192.168.1.42
```

The console listens on TCP 8092; this client listens for video on UDP 9445. Both need to be
reachable on your LAN.

## Reading the log

Once a second the client prints a line like:

```
14 fps (14 decoded)  7.31 Mbit/s  incomplete=3 crcfail=0 ignored=0
```

- **incomplete** — frames that lost at least one datagram and were discarded. A few is normal on
  Wi-Fi; a lot means the network, not the console, is the bottleneck.
- **crcfail** — frames that arrived complete but corrupt. Should be zero.
- **ignored** — datagrams that were not valid v2 packets. If this is climbing and nothing ever
  decodes, the console is running the wrong plugin version.

## Building

```bash
mvn -B package
```

The runnable jar lands in `target/`.

## What changed from upstream

- **Lombok removed.** Upstream pinned `1.16.20`, which cannot run on any JDK newer than 8 — the
  build simply fails on a current toolchain. The two annotations in use (`@Log`, `@Synchronized`)
  were a few lines each to write by hand.
- **Protocol v2 receiver** (`FrameAssembler`). Every datagram is now self-describing, so a lost
  packet costs one frame instead of desynchronising the stream. Every length is bounds-checked
  before it can size an allocation — upstream did `new byte[curJPEGSize]` straight from the wire,
  where a desync threw `NegativeArraySizeException` or `OutOfMemoryError` and killed the receive
  thread permanently.
- **4 MB UDP receive buffer.** A 720p frame is ~100 datagrams arriving back to back; the default
  buffer holds about 45 of them, so the tail of every frame was being dropped by the OS before the
  client ever saw it. The client warns if the OS refuses the larger size.
- **JPEG decoding moved off the receive thread** onto a depth-1 drop-oldest queue. Decoding inline
  parked the socket for the duration of every decode and cost the start of the next frame.
- **Aspect ratio is preserved.** Upstream stretched the frame to fill the window and sized that
  window to the whole screen minus a fixed margin, which produced a window taller than the desktop
  on laptops.
- `TCPClient` now sets a read timeout — without one, a console that stops answering parked the
  heartbeat thread in `readByte()` forever and the client never noticed it had disconnected.
- Per-second stats, so you can tell a console-side bottleneck from a network one.

## Licence

MIT, as upstream.
