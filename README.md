# 🌙 Moonlight 360

[![Stand With Ukraine](https://raw.githubusercontent.com/vshymanskyy/StandWithUkraine/main/badges/StandWithUkraine.svg)](https://stand-with-ukraine.pp.ua)
[![Release](https://img.shields.io/github/v/release/everythings-gonna-be-alright/moonlight360?include_prereleases&label=release)](https://github.com/everythings-gonna-be-alright/moonlight360/releases)
[![Downloads](https://img.shields.io/github/downloads/everythings-gonna-be-alright/moonlight360/total)](https://github.com/everythings-gonna-be-alright/moonlight360/releases)
[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue)](LICENSE)
![Platform: Xbox 360 RGH/JTAG](https://img.shields.io/badge/platform-Xbox%20360%20RGH%2FJTAG-107C10)

**Stream your PC games to an Xbox 360.** 🎮 Yes, the 2005 one. Up to **1080p at 60 fps**.

An unofficial [Moonlight](https://moonlight-stream.org) client for RGH/JTAG consoles. It works with
[Sunshine](https://github.com/LizardByte/Sunshine) and its forks (Apollo, Vibeshine), and is built with the
open-source [xsdk](https://github.com/xenosworks/xsdk): no Microsoft XDK involved.

![A tour of the menus](docs/screenshots/v0.3/tour.webp)

## ✨ Features

- Pair, pick an app, play. Launch, resume and quit games on the PC.
- H.264 video from 360p to 1080p, at 30 or 60 fps. Opus stereo audio. Controllers 1–4 with rumble.
- A true 1080p picture on a 1080p TV (*Video → 1080p picture*), not 720p scaled up by the console.
- A stats HUD: frame rate, latency by stage, ping, packet loss, CPU and GPU load and temperatures. 🥵
- Low-latency display, and controllers read 500 times a second.
- Mouse and keyboard from the controller: a mouse mode, typing with the on-screen keyboard, and keys like Esc,
  Alt + Tab and Guide, all in the stream menu (**X** there switches mouse mode, **Y** types).
- Finds your PC on the network, shows app covers, and keeps your recent apps one press away.
- Wake-on-LAN, reconnect on start, and four colour themes.
- A menu made for a controller and a TV.

Not supported: HEVC, AV1 and HDR. The console is older than all three.

## 🚀 Quick start

You need an RGH/JTAG console with a homebrew dashboard (Aurora, FreeStyle Dash or XeXMenu), and a PC running
Sunshine, Apollo or Vibeshine on the same network.

1. **Download** `moonlight360.xex` from
   [Releases](https://github.com/everythings-gonna-be-alright/moonlight360/releases).
2. **Copy** it to a folder on the console, for example `Hdd1:\Apps\Moonlight360\` (FTP or a FAT32 USB stick), and
   launch it.
3. **Set the PC.** On the *Play* tab, set *Host PC* to the PC's IP address.
4. **Pair, once.** Pick *Start streaming* and type the 4-digit PIN from the console into the host's web UI
   (`https://localhost:47990` → *PIN*).
5. **Play.** Hold **Back** for a second to open the stream menu (another button or combination can be set in
   *Other → Stream menu*).

Settings are saved to `moonlight.ini` next to the `.xex`. If something goes wrong, `moonlight360.log` says why.

## ⚡ Best settings and performance

> [!WARNING]
> **Wi-Fi has problems right now.** Over the console's Wi-Fi a stream can stop, and the console then stops
> receiving anything until it's rebooted. The cause isn't found yet, so there's no fix. Use Ethernet for now.

### Recommended

**1080p · 60 fps · 30 Mbit/s, with a lean stream.** 🪶

- **Measured:** a six-minute stream averaged 59 fps, with 40 ms from the PC to the screen.
- **Trade-off:** the PC's processor does the encoding, and the picture is a little blockier.

### Lean stream setup

A lean stream leaves out the deblocking filter and sub-pixel motion: half of the console's decoding work.

**On the PC** (the host's web UI):

1. *Force a Specific Encoder* → **Software**
2. *Software Encoder* → *SW Presets* → **ultrafast**

**On the console:**

3. **1920×1080**, **60 fps**, *Fast decode* off, *1080p picture* on (restart the app once after turning it on).
4. Start a new stream. The stats HUD shows `CAVLC lean` when it worked.

### No spare CPU on the PC?

Keep the hardware encoder, with *Prefer CAVLC over CABAC* on:

- 720p or 1600×900 at 60 fps
- 1080p at 30 fps, with *Fast decode* on

### Decode time per 1080p frame

60 fps needs under 16.7 ms a frame, 30 fps under 33 ms.

| Stream | On the PC | *Fast decode* | at 30 fps | at 60 fps |
|---|---|---|---:|---:|
| 🪶 **Lean** | Encoder **Software**, preset **ultrafast** | off | 17 ms | **12 ms** |
| Hardware encoder | *Prefer CAVLC over CABAC* | on | 20 ms | 15 ms |
| Hardware encoder | *Prefer CAVLC over CABAC* | off | 30 ms | 23 ms |

### Experimental

- 🧪 **MPEG-2** (*Video* → *Video codec*): needs Sunshine with `mpeg2_mode = 2`; untested on a real host.

### Under the hood

- FFmpeg decodes on all six hardware threads.
- Its vector code is patched for the 360's CPU (`patches/ffmpeg-xenon-vmx128.patch`).
- Each frame comes as 24 slices, shared between the threads.

## 🛠️ Building

Everything builds in Docker, on Linux or macOS.

```sh
git clone --recursive https://github.com/everythings-gonna-be-alright/moonlight360.git && cd moonlight360
docker build -t moonlight-xsdk-toolchain -f docker/xsdk-toolchain.dockerfile third_party/xsdk/tools/toolchain  # ~10 min, once

run() { docker run --rm -v "$PWD":/work moonlight-xsdk-toolchain /work/$1; }
run scripts/build-xsdk.sh
run scripts/build-ffmpeg.sh
run scripts/build-app.sh      # -> build/app/dist/moonlight360.xex
```

## 🧰 Developer tools (`dev/`)

Not needed to build or use the app.

- **`devlaunch` + `devserver.py`:** send builds to the console over the network and get the logs back.
- **`bench/`:** `decbench`, the console benchmark behind the table above.
- **`vmx_test/`:** checks the 360 vector code against FFmpeg's C code under QEMU, with no console.
- **`menu_preview/`:** renders the menus to PNG on a PC. The screenshots come from it.
- **`make_clips.sh`:** makes the benchmark's test clips.

## 🙏 Credits and license

Built on [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c),
[moonlight-tv](https://github.com/mariotaku/moonlight-tv)'s libgamestream, [xsdk](https://github.com/xenosworks/xsdk),
FFmpeg, Opus, mbedTLS, curl and expat. [Free60](https://free60.org) documented the hardware.

GPLv3 (see [LICENSE](LICENSE)); the submodules keep their own licenses. Not affiliated with Microsoft or Moonlight.
Xbox 360 is a trademark of Microsoft.
