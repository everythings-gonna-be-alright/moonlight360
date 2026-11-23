# 🌙 Moonlight 360

**Stream your PC games to an Xbox 360.** 🎮 Yes, the 2005 one.

An unofficial port of [Moonlight](https://moonlight-stream.org) to the Xbox 360. It works with
[Sunshine](https://github.com/LizardByte/Sunshine) and its forks (Apollo, Vibeshine). It needs an **RGH/JTAG
console** and runs as a regular `.xex` built with the open-source [xsdk](https://github.com/xenosworks/xsdk), with no
Microsoft XDK involved.

| | |
|---|---|
| ![Main menu](docs/screenshots/play.png) | ![Video settings](docs/screenshots/video.png) |
| ![Pairing](docs/screenshots/pairing.png) | ![Stats HUD](docs/screenshots/stats.png) |

## ✨ Features

- Pairing, the app list, and launching, resuming and quitting games on the PC.
- H.264 video at 720p, 1600x900 or 1080p30; Opus stereo audio; controllers 1–4 with rumble.
- A menu made for a controller and a TV.
- A stats HUD, compact or detailed: frame rate, latency split into PC, network, decode and display, ping, packet
  loss, CPU and GPU load and temperatures, with live graphs. Now you can watch your console sweat in real time. 🥵
  Switch it from the stream menu (Back + Start).
- Low-latency display (Video tab) and controllers read 1000 times a second, for less lag.
- Experimental GPU decoding (see [below](#-gpu-decoding-beta)).

Not supported: HEVC, AV1 and HDR. The console is older than all three.

## 🚀 Running a release

You need:
- an RGH/JTAG Xbox 360 with a homebrew dashboard (Aurora, FreeStyle Dash or XeXMenu);
- a PC with [Sunshine](https://github.com/LizardByte/Sunshine) (or Apollo or Vibeshine) installed and running;
- both on the same network.

1. **Download** `moonlight360.xex` from
   [Releases](https://github.com/everythings-gonna-be-alright/moonlight360/releases).
2. **Copy it to the console,** into a folder of its own, for example `Hdd1:\Apps\Moonlight360\`. Use the dashboard's
   FTP server (Aurora has one built in) or a FAT32 USB stick.
3. **Launch it.** In Aurora, add the folder to the content scan paths and the app shows up with your other
   homebrew. Or open the folder in a file manager (Aurora's, or XeXMenu) and start `moonlight360.xex`.
4. **Point it at your PC.** On the *Play* tab, select *Host PC* and type the PC's IP address.
5. **Pair, once.** Pick *Start streaming*. The console shows a 4-digit PIN: open Sunshine's web UI on the PC
   (`https://localhost:47990`), go to the *PIN* page and enter it.
6. **Play.** Hold **Back + Start** for a second while streaming to open the stream menu: resume, go back to the
   main menu (the game keeps running) or quit the game.

Settings are saved to `moonlight.ini` next to the `.xex`; you can also copy the example `moonlight.ini` from the
release and edit `host=` there. If something goes wrong, `moonlight360.log` in the same folder says why.

## 👍 Recommended setup

- 🔌 **Connect the console by Ethernet.** Over Wi-Fi, the console can stop receiving network traffic in the middle of
  a stream, and only a reboot brings it back. We don't know exactly why. In our tests, fewer and larger packets
  helped, so the app asks for 20 ms audio packets instead of 5 ms and only offers 1392- and 1024-byte video packets.
  We can't promise that's a fix. Ethernet never had the problem.
- 🎞️ **Codec: H.264 with CAVLC entropy coding.** H.264 is the only codec the 360 can decode. In Sunshine's encoder
  settings, set the H.264 coder to **CAVLC** instead of CABAC. CAVLC takes about 40% less time to decode, and at
  1080p that's the difference between smooth and stuttering.
- 📺 **Resolution and bitrate:** 720p at 8–10 Mbit/s is the safe choice. For 1080p30 at up to 25 Mbit/s you need
  Ethernet, CAVLC and *Fast decode* on.
- 🐢 **Desktop stuck at 30 fps?** Desktop capture only sends a frame when the screen changes. A game will run at 60.

## ⚡ Performance

1080p30 clips at about 22 Mbit/s, decoded on the console. Milliseconds per frame (30 fps needs 33 or less):

| Decoder setup | CABAC | CAVLC |
|---|---:|---:|
| FFmpeg in C, 4 threads on 2 cores (where we started) | 89 | – |
| FFmpeg in C, 6 threads on 3 cores | 65 | 44 |
| + AltiVec, patched for Xenon | 54 | 33 |
| + no deblocking (*Fast decode*) | 43 | **23** |

Each frame is checked against FFmpeg on a PC. Xenon lacks AltiVec's vector integer multiplies, but the compiler
emits them anyway, so `patches/ffmpeg-xenon-vmx128.patch` replaces them and the build fails if any are left.

## 🧪 GPU decoding (beta)

The 360 has no hardware video decoder, so the hybrid decoder splits the work:
1. The CPU parses the stream.
2. The GPU's shaders do motion compensation and add the residual (`app/src/gpumc.c`, `app/shaders/mc_*`).
3. The CPU rebuilds the intra blocks.

The output is bit-exact with FFmpeg, but it's **not faster yet**: about 33 ms a frame against 19 ms for the CPU
alone. If it fails during a stream, the app switches back to the CPU decoder.

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

The patches in `patches/` are applied by the build.

## 🧰 Developer tools (`dev/`)

None of these are needed to build or use the app.

- **`devlaunch` + `devserver.py`:** run new builds on the console from the PC and get the logs back, with no FTP:
  1. Put `devlaunch.xex` on the console, with a `devlaunch.txt` next to it containing `<PC IP> 7380`.
  2. Run `python3 dev/devserver.py` on the PC.
  3. Run `python3 dev/devserver.py --bump` to send each new build.
- **`bench/`:** console benchmarks. `decbench` produced the table above; `hybench` measures the GPU decoder.
- **`hybrid_test/`:** checks the GPU decoder's maths against FFmpeg on a PC, in seconds.
- **`menu_preview/`:** renders the menus to PNG on a PC. The screenshots above come from it.
- **`make_clips.sh`:** makes the test clips, the same on every machine.

Build scripts for these are in `dev/scripts/`.

## 🙏 Credits and license

Built on [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c),
[moonlight-tv](https://github.com/mariotaku/moonlight-tv)'s libgamestream, [xsdk](https://github.com/xenosworks/xsdk),
FFmpeg, Opus, mbedTLS, curl and expat. The texture-tiling code comes from [Xenia](https://github.com/xenia-project/xenia),
and [Free60](https://free60.org) documented the hardware.

GPLv3 (see [LICENSE](LICENSE)); the submodules keep their own licenses. This project isn't affiliated with Microsoft
or Moonlight. Xbox 360 is a trademark of Microsoft.
