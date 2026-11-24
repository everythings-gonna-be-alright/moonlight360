# 🌙 Moonlight 360

**Stream your PC games to an Xbox 360.** 🎮 Yes, the 2005 one. Up to **1080p at 60 fps**.

An unofficial [Moonlight](https://moonlight-stream.org) client for RGH/JTAG consoles. It works with
[Sunshine](https://github.com/LizardByte/Sunshine) and its forks (Apollo, Vibeshine), and is built with the
open-source [xsdk](https://github.com/xenosworks/xsdk): no Microsoft XDK involved.

| | |
|---|---|
| ![Main menu](docs/screenshots/play.png) | ![Video settings](docs/screenshots/video.png) |
| ![Pairing](docs/screenshots/pairing.png) | ![Stats HUD](docs/screenshots/stats.png) |

## ✨ Features

- Pair, pick an app, play. Launch, resume and quit games on the PC.
- H.264 video from 360p to 1080p, at 30 or 60 fps. Opus stereo audio. Controllers 1–4 with rumble.
- A stats HUD: frame rate, latency by stage, ping, packet loss, CPU and GPU load and temperatures. 🥵
- Low-latency display, and controllers read 500 times a second.
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
5. **Play.** Hold **Back** for a second to open the stream menu.

Settings are saved to `moonlight.ini` next to the `.xex`. If something goes wrong, `moonlight360.log` says why.

## ⚡ Best settings and performance

**Recommended: 1080p at 60 fps with a lean stream.** 🪶

Time to decode one 1080p frame on the console, at about 22 Mbit/s. 60 fps needs under 16.7 ms a frame, 30 fps under
33. A 60 fps frame decodes faster because it carries half the bits of a 30 fps frame at the same bitrate.

| Stream | On the PC | *Fast decode* | 1080p30 frame | 1080p60 frame |
|---|---|---|---:|---:|
| 🪶 **Lean** | Encoder **Software**, preset **ultrafast** | off | 17 ms | **12 ms** |
| Hardware encoder | *Prefer CAVLC over CABAC* | on | 20 ms | 15 ms |
| Hardware encoder | *Prefer CAVLC over CABAC* | off | 30 ms | 23 ms |

**Lean stream setup.** It leaves out the deblocking filter and sub-pixel motion, half of the console's decoding work.

1. Host's web UI: *Force a Specific Encoder* → **Software**
2. *Software Encoder* → *SW Presets* → **ultrafast**
3. Console: **1920×1080**, **60 fps**, *Fast decode* off. Start a new stream.

The stats HUD shows `CAVLC lean` when it worked. A real six-minute 1080p60 stream at 30 Mbit/s averaged 59 fps and
40 ms from the PC to the screen. The price: the PC's processor encodes, and the picture is a little blockier.

**No spare CPU on the PC?** Keep the hardware encoder: 720p or 1600×900 at 60 fps, or 1080p at 30 with *Fast decode*.

- 🔌 **Ethernet.** Over the console's Wi-Fi a stream can stop receiving until you reboot.
- 📶 **Bitrate.** 8–10 Mbit/s at 720p, up to 30 at 1080p.
- 🐢 **Desktop below 60 fps?** Desktop capture only sends a frame when the screen changes. Games run at 60.
- 🧪 **MPEG-2** (*Network* → *Video codec*): experimental, needs Sunshine with `mpeg2_mode = 2`, untested on a real host.

Under the hood: FFmpeg on all six hardware threads, vector code patched for the 360's CPU
(`patches/ffmpeg-xenon-vmx128.patch`), 24 slices a frame shared between threads.

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
