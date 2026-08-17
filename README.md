# ScreenStreaming for the Wii U — 720p fork

A fork of [Maschell/StreamingPluginWiiU](https://github.com/Maschell/StreamingPluginWiiU) that
streams the console's **TV (HDMI) output at 720p** and runs on Aroma.

Upstream is a 2018 proof of concept. It builds a `.mod` for a plugin loader that no current
Wii U setup can load, and its resolution options never worked (see below). This fork ports it
to the modern plugin system, fixes the capture path, and replaces the wire protocol so that a
720p frame — roughly 100 UDP datagrams instead of 25 — survives the trip.

> **Status: builds in CI, not yet verified on hardware.** Every change here is derived from the
> current WUPS/wut headers and from Maschell's own actively-maintained
> [ScreenshotWUPS](https://github.com/wiiu-env/ScreenshotWUPSPlugin), which captures the same
> buffers the same way. It has not been run on a real console. Treat the first run as a bring-up:
> build with `DEBUG=1` and watch the UDP log.

## Requirements

- A Wii U running **[Aroma](https://aroma.foryour.cafe/)**
- The **MemoryMappingModule** that ships with Aroma (the capture buffers need GX2-addressable
  memory — a normal `malloc` returns memory the GPU cannot write to)
- The **matching client from this fork** — the wire protocol changed, see [PROTOCOL.md](PROTOCOL.md).
  The upstream client will show a black window.

Copy `screenstreaming.wps` to:

```
sd:/wiiu/environments/aroma/plugins/
```

## Usage

1. Boot into Aroma and start a game.
2. On your PC, run the client and enter the console's IP address.
3. Configure the plugin from the Aroma config menu (**L + DPAD DOWN + MINUS**).

| Setting | Default | Notes |
| --- | --- | --- |
| Screen to stream | TV | TV is the HDMI output. |
| Resolution | Native | See "About resolution" below. |
| JPEG quality | 55 | 10–95. Lower it if your Wi-Fi is the bottleneck. |
| Skip N frames between captures | 1 | 1 means capture every other frame. Raise it if the game stutters. |
| Colour | Auto | Auto gamma-corrects when the game's scan buffer is sRGB. |
| Encoder core | 2 | Move it if the game you play is busy on core 2. |

## About resolution

**Capture is always 1:1 from whatever the game renders.** That is not a limitation, it is the
only correct way to do it:

> *Cafe SDK 2.04 release notes — Breaking changes:*
> "Using `GX2CopySurface` to copy between two different formats or **surface dimensions** has been
> removed."

Upstream asked `GX2CopySurface` for an 854×480 destination from a 1280×720 source on every frame.
When the dimensions disagree the copy does nothing at all and leaves the destination holding
whatever was in the freshly allocated memory — which is why upstream's README says "some games
might be too dark, some might be too bright, some doesn't work at all". Its 240p/360p/480p options
were never resizing anything.

So: the GPU copies at native size, and the **CPU** downscales afterwards if you asked for something
smaller. Since practically every Wii U game renders its TV output at exactly 1280×720, `Native`
gives you real 720p. Downscaling never upscales: a game that renders 854×480 stays 854×480 on any
setting whose cap is larger, and the client scales it for display.

The CPU pass is skipped entirely — the captured surface goes straight into the JPEG encoder — when
the requested size is at least the source size **and** no colour conversion is needed. Note the
second half: with `Colour = Auto` (the default) an sRGB title always takes the pass, because the
gamma correction has to happen somewhere. That path is a straight per-pixel table lookup with no
resampling, so it is much cheaper than a real downscale, but it is not free. Set `Colour = Raw` to
skip it at the cost of a too-dark picture on those titles.

A game rendering at 1080p is captured at 1080p regardless of this setting, because capture is
always 1:1 — the setting only decides what the encoder produces. That needs ~8.3 MB per capture
buffer; if the console cannot spare it, the log says so and the stream stays black. There is no
automatic fallback, because a smaller GPU destination would copy nothing at all.

## What to expect

Honest numbers, because this matters more than the resolution setting:

- The Espresso CPU has no AltiVec, so libjpeg-turbo runs its **plain C** path. There is no hardware
  JPEG encoder in play.
- A 1280×720 frame is 3.6 MB of RGBA to compress on one 1.24 GHz in-order core.
- Expect roughly **8–15 fps at 720p** with a game running, and a noticeable hit to that game.
  480p is far smoother. This is a screen-sharing tool, not a low-latency capture card.
- Bandwidth at 720p/q55 is roughly 6–12 Mbit/s, which is near the practical ceiling of the
  console's 2.4 GHz 1x1 Wi-Fi. A USB Ethernet adapter helps a lot.

If what you actually want is to watch your Wii U on a PC, [vanilla](https://github.com/vanilla-wiiu/vanilla)
does 480p60 by speaking the real GamePad protocol, needs no CFW, and will beat this on both
latency and smoothness. This fork is for capturing the *TV* output at *720p*, which vanilla cannot do.

## What changed from upstream

**Ported to the current plugin system**
- WUPS 0.9 / Aroma. Output is `screenstreaming.wps`, not `.mod`.
- `WUPS_GET_CONFIG` is a hard `static_assert` in modern WUPS; the menu is rebuilt on `WUPSConfigAPI`,
  with settings persisted through `WUPSStorageAPI`.
- `ON_APP_STATUS_CHANGED` no longer exists; foreground tracking moved to
  `ON_ACQUIRED_FOREGROUND` / `ON_RELEASE_FOREGROUND`.
- Dropped `libutilswut` (gone). The TCP server, the logger and the worker threads are implemented
  directly against wut.
- Dropped the 2018 prebuilt `libs/libturbojpeg.a` in favour of devkitPro's portlib.

**Capture**
- Copy at source dimensions, always — the resize is done on the CPU (see above).
- Capture buffers are allocated **once** and recycled. Upstream did a `memalign`/`free` of the whole
  RGBA surface every frame: 220 MB/s of allocator traffic at 720p, which fragments the heap until
  allocation starts failing and frames silently disappear.
- The buffer is claimed *before* the GPU copy. Upstream did the full copy and a `GX2DrawDone()`
  pipeline stall on the game's render thread and only then asked whether the encoder had room,
  paying the entire cost of every frame it discarded.
- `DCInvalidateRange` before the encoder reads, executed **on the encoder's own core** — `dcbi`
  only affects the cache of the core that runs it, so doing it in the GX2 hook would both fail to
  help the reader and put a 115k-block cache walk on the game's render thread.
- Also hooks `GX2MarkScanBufferCopied`/`GX2GetCurrentScanBuffer` for games that never call
  `GX2CopyColorBufferToScanBuffer`, and `GX2SetTVBuffer`/`GX2SetDRCBuffer` to learn whether the
  scan-out is sRGB.

**Encoding**
- One `tjhandle` and one output buffer for the whole session instead of `tjInitCompress`/`tjDestroy`
  and a fresh allocation per frame.
- 4:2:0 instead of 4:1:1, and `TJFLAG_FASTDCT` — the DCT dominates on a CPU with no SIMD.
- When no resize and no colour conversion are needed, the captured surface is fed to the encoder
  with zero intermediate passes; the 1:1 colour-correction path is a table lookup with none of the
  area-averaging arithmetic.
- Removed the adaptive-quality controller. It could only ever ratchet down: its recovery branch
  required dropping fewer than 20% of frames, which never happens, so it pinned quality at the
  floor one second in and stayed there. Quality is a plain setting now, because on a CPU-bound
  encoder lowering quality buys bitrate, not frame rate.

**Networking**
- New self-describing packet format ([PROTOCOL.md](PROTOCOL.md)). Upstream inferred frame structure
  from datagram *size* and *arrival order*, so one lost packet corrupted the next frame too, and a
  desync could drive the client into `new byte[<garbage>]` and kill its receive thread for good.
- `SO_SNDBUF` raised, and congested sends retry instead of silently truncating the frame.

**Lifetime and threading**
- The UDP socket is behind a mutex. Upstream deleted it from the TCP thread while the encoder
  thread was mid-send.
- Teardown clears the capture gate, waits for an in-flight capture on the game's render thread to
  finish, stops the threads, and only then frees the buffers.
- `EncodingHelper` never deleted its `CThread`, leaking a 256 KB stack on every settings change;
  its `pThread` was also read uninitialised by `setThreadPriority`.
- `MJPEGStreamServerUDP`'s constructor returned early when `socket()` failed, leaving the CRC table
  and message queue uninitialised — and then used them.

## Building

With Docker (nothing else needed on your machine):

```bash
docker build . -t screenstreaming-builder
```

```bash
docker run -it --rm -v ${PWD}:/project screenstreaming-builder make
```

For the debug build with live logging over UDP (use `wiiulogserver` or the Aroma logging module):

```bash
docker run -it --rm -v ${PWD}:/project screenstreaming-builder make DEBUG=1
```

Without Docker you need devkitPPC, [wut](https://github.com/devkitPro/wut),
[WUPS](https://github.com/wiiu-env/WiiUPluginSystem),
[libmappedmemory](https://github.com/wiiu-env/libmappedmemory) and the `wiiu-libjpeg-turbo`
portlib, then just `make`.

## Licence

GPL-3.0, as upstream.
