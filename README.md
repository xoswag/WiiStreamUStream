# WiiStreamUStream

Stream a Wii U's **TV (HDMI) output at 1280x720** to a PC over the network.

This is a fork of Maschell's two-part streaming tool, ported to the modern Aroma
environment and reworked so that 720p actually holds together:

| | Upstream | Here |
| --- | --- | --- |
| Max resolution | 854x480 | **1280x720** |
| Plugin system | WUPS 0.1 (`.mod`, 2018 loader) | **WUPS 0.9 / Aroma (`.wps`)** |
| Wire protocol | unlabelled datagram bursts | **self-describing chunks (v2)** |
| Client build | Lombok 1.16.20, JDK 8 only | **plain Java 17** |

Both halves must come from this repository — the wire protocol is not compatible
with upstream's. See [plugin/PROTOCOL.md](plugin/PROTOCOL.md).

## Layout

* **[`plugin/`](plugin/)** — the Wii U plugin. Builds to `wiistreamustream.wps`,
  goes in `sd:/wiiu/environments/aroma/plugins/`. GPL-3.0, from
  [Maschell/StreamingPluginWiiU](https://github.com/Maschell/StreamingPluginWiiU).
* **[`client/`](client/)** — the desktop viewer. Builds to a runnable jar. MIT, from
  [Maschell/StreamingPluginClient](https://github.com/Maschell/StreamingPluginClient).

Both upstream histories are preserved in this repository via `git subtree`.

## Quick start

1. Grab `wiistreamustream.wps` and the client jar from the latest CI run, or build
   them yourself (see each subproject's README).
2. Copy the `.wps` to `sd:/wiiu/environments/aroma/plugins/`. **MemoryMappingModule**
   must be present in `sd:/wiiu/environments/aroma/modules/` — capture buffers have to
   come from GX2-addressable memory.
3. Boot Aroma, start a game, and configure the plugin from the Aroma plugin menu
   (screen, resolution, quality, frame skip).
4. Run the client, enter the console's IP.

## Honest status

The port compiles against the current WUPS/wut headers *on paper* — it has been
reviewed line by line against them, but it has **not yet been through a compiler or
run on hardware**. Expect to iterate on the first CI run. Two things in particular
still need a human:

* **The pinned Docker image tags** in `plugin/Dockerfile`. If `docker build` fails on
  line 1, bump the datestamps.
* **The sRGB conversion direction.** `Colour = Auto` should fix the too-dark output
  upstream had. If the picture comes out washed out instead, the conversion is
  inverted.

Expect roughly 8-15 fps at 720p. The encoder is software JPEG on a 1.24 GHz PowerPC
core with no SIMD — that is the ceiling, and it is why frame skip exists.
