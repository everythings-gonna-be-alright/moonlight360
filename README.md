# Moonlight 360

**Stream your PC games to an Xbox 360.** Yes, the 2005 one. Yes, really.

This is an unofficial port of the [Moonlight](https://moonlight-stream.org) game-streaming client to the Xbox 360.
It talks to [Sunshine](https://github.com/LizardByte/Sunshine) and its forks (Apollo, Vibeshine) and plays your
PC games, with sound and up to four controllers, on a console older than some of the people reading this.

Moonlight 360 needs an **RGH/JTAG console** with Aurora or another homebrew dashboard. It's a regular `.xex`
built with the open-source [xsdk](https://github.com/xenosworks/xsdk), so no Microsoft XDK is involved, leaked or
otherwise.

| | |
|---|---|
| ![Main menu](docs/screenshots/play.png) | ![Video settings](docs/screenshots/video.png) |
| ![Pairing](docs/screenshots/pairing.png) | ![Stream menu](docs/screenshots/stream-menu.png) |

<sub>These are renders of the real menu code made on a PC by `dev/menu_preview`. The console draws the same
pixels; photographing a TV with a phone was the alternative.</sub>

## What works

- **Pairing, the app list, launching, resuming and quitting** apps on the PC.
- **H.264 video:**
  - 720p at 8 Mbit/s is comfortable;
  - 1080p30 at 25 Mbit/s works if the host encodes with CAVLC (see below);
  - 1600x900 is the in-between option.
- **Opus stereo audio** and **controllers 1–4**, rumble included. Your controller finally has a reason to exist again.
- **A menu built for a controller and a TV,** with four tabs (Play, Video, Network, Debug), an on-screen keyboard
  for the IP address, and a help line under every setting.
- **A stats overlay:** decode time, frames shown, dropped frames and errors, load on each of the six hardware
  threads, GPU load, and the CPU, GPU, eDRAM and board temperatures. Now you can watch your console sweat in real
  time.
- **Experimental GPU decoding** (Video → *GPU decoding (beta)*). See [below](#the-gpu-decoder-or-teaching-a-2005-gpu-h264).

![Stats overlay](docs/screenshots/stats.png)

## What doesn't (yet)

- **HEVC and AV1.** The Xbox 360 predates both by about a decade. H.264 only.
- **CABAC at 1080p.** The CPU can't keep up with it; see [Performance](#performance).
- **Cold-boot networking for `devlaunch`.** When `devlaunch` is the boot program, the network sometimes doesn't
  come up until Aurora has run once. This only affects the development launcher, not the app.
- **HDR.** It's 2005. Every range is standard.

## Quick start

1. **Download `moonlight360.xex`** from [Releases](https://github.com/everythings-gonna-be-alright/moonlight360/releases),
   or build it yourself (see [Building](#building)).
2. **Copy it** to its own folder on the console, over FTP or a USB stick.
3. **Launch it from Aurora.** On the **Play** tab, set *Host PC* to your PC's IP address.
4. **Pick *Start streaming*.** The first time, the app shows a 4-digit PIN: enter it on Sunshine's web UI, on the
   PIN page. You only do this once.
5. **Play.** Hold **Back + Start** for a second to open the stream menu. From it you can go back to the main menu
   (the game keeps running on the PC) or quit the game.

Settings are saved to `moonlight.ini` next to the `.xex`. The log is `moonlight360.log`, in the same place, for when
things go sideways.

### Recommended host settings

- **Encoder:** software (x264), or any encoder that can do **CAVLC**. In Sunshine, set *Advanced → H.264 entropy
  coding → CAVLC*. CAVLC takes about 40% less time to decode on the 360, and at 1080p that's the whole difference.
- **Over Wi-Fi:** 720p at 6–10 Mbit/s.
- **Over Ethernet:** up to 25–30 Mbit/s.
- **Desktop at 30 fps when you asked for 60?** That's normal. Desktop capture sends a frame only when something on
  screen changes. Start a game and you'll get your 60.

## Performance

Measured on the console. The clips are 1080p30 at about 22 Mbit/s, shaped like a Sunshine stream. Times are
**milliseconds per frame**, and lower is better. A 30 fps stream gives you 33 ms per frame.

| Decoder setup | CABAC | CAVLC |
|---|---:|---:|
| FFmpeg in C, 4 threads on 2 cores (where we started) | 89 | – |
| FFmpeg in C, 6 threads on all 3 cores | 65 | 44 |
| + AltiVec | 54 | 33 |
| + AltiVec, no deblocking (*Fast decode*) | 43 | **23** |

Every frame of every run is checked against FFmpeg on a PC, so speed never comes at the cost of a wrong pixel.

What got it from "slideshow" to "playable":

- **All three cores, six hardware threads,** and the host asked for six slices per frame, so each thread gets one.
- **FFmpeg's AltiVec H.264 code, patched for Xenon.** The 360's VMX128 lacks AltiVec's vector integer multiplies
  (`vmladduhm`, `vmsum*` and friends). The compiler emits them anyway, and they fail on the console. So
  `patches/ffmpeg-xenon-vmx128.patch` rewrites the qpel filters' multiplies by 20 and 5 as shifts and adds. The
  build then disassembles FFmpeg and fails if a single forbidden instruction is left.
- **Decoding straight into GPU-visible memory,** so the GPU draws the frame where FFmpeg left it, with no 6 ms copy.
- **Frame threading** was tried and dropped. It was 10% faster on average, but it delivered frames in bursts 150–270 ms
  apart. Averages don't play games; frame times do.

## The GPU decoder, or teaching a 2005 GPU H.264

The Xbox 360 has no video decoder, and FFmpeg pins all three CPU cores at 1080p. Meanwhile the Xenos GPU sits
there drawing one rectangle per frame. So Moonlight 360 has a **hybrid decoder**, enabled under *GPU decoding
(beta)*:

1. **The CPU parses** the H.264 stream (FFmpeg with a hook in `libavcodec/h264_xenon.c`). It records each
   macroblock's motion vectors and residuals into textures instead of reconstructing the picture.
2. **The GPU predicts.** Shaders run the H.264 6-tap and bilinear interpolation for luma and chroma, then add the
   residual (`app/src/gpumc.c`, `app/shaders/mc_*.frag`).
3. **The CPU replays** only the intra macroblocks, which need their already-decoded neighbours, on six threads.

It is **bit-exact**: the output matches FFmpeg's to the last pixel on every frame of every test clip, both in the
PC test (`dev/hybrid_test`) and on the console (`hybench`). It is **not faster yet**. At 1080p the GPU part takes
about 33 ms, against 19 ms for the CPU decoding the same frame by itself. Next on the list:

- keep the frames on the GPU instead of untiling them back for the CPU (10 ms);
- make the two slowest passes cheaper.

That's why it's labelled beta. If anything fails mid-stream, the app falls back to the CPU and asks the host for a
keyframe, and you'll see a glitch rather than a crash.

### Things we learned the hard way

- **The Wi-Fi rule.** Moonlight normally asks for 5 ms audio packets, which is about 300 tiny packets a second.
  Within a second, the 360's Wi-Fi adapter stops receiving anything until a **reboot**. The link stays up and the
  console still transmits; it just goes deaf. So Moonlight 360 asks for 20 ms packets and allows only large video
  packets (1392 or 1024 bytes). Nobody knows why the adapter does this. It took a relay that held back one stream
  at a time to find out.
- **The shader compiler sometimes lies.** xsdk's GLSL compiler (xost) silently compiles `mat4 * vec4` into an
  empty program, and a pixel shader that never writes its colour hangs the GPU. `scripts/check_shaders.py` now fails
  the build on any shader that comes out empty.
- **Vector `max()` and `length()` miscompile too.** That's where the mysterious diamond pattern behind the menus
  came from. The shape shader is now written one scalar at a time, and the diamonds are gone.
- **mbedTLS 3 has no default CBC padding mode,** so audio decryption failed on every packet until
  `patches/moonlight-common-c-mbedtls3-cbc-padding.patch` set PKCS#7 explicitly. This one wasn't the Xbox's fault.

## Building

Everything builds in Docker, on Linux or macOS (Apple Silicon is fine).

```sh
git clone --recursive https://github.com/everythings-gonna-be-alright/moonlight360.git
cd moonlight360

# The xsdk GCC 15 toolchain. This takes about 10 minutes, once. Good time for a coffee.
docker build -t moonlight-xsdk-toolchain -f docker/xsdk-toolchain.dockerfile third_party/xsdk/tools/toolchain

run() { docker run --rm -v "$PWD":/work moonlight-xsdk-toolchain /work/$1; }
run scripts/build-xsdk.sh     # the SDK
run scripts/build-ffmpeg.sh   # FFmpeg with the Xenon AltiVec patch and the GPU decoder hook
run scripts/build-app.sh      # -> build/app/dist/moonlight360.xex
```

Forgot `--recursive`? Run `git submodule update --init --recursive`.

The build applies the patches in `patches/` to the submodules by itself, and skips any that are already applied.

That's all you need for the app. Everything else lives in `dev/`.

## Developer tools (`dev/`)

You don't need anything in `dev/` to build or use Moonlight 360. It's what we used to make the app, kept for
anyone who wants to work on it. Here's what each tool is for.

**Running code on the console without walking to it: `devlaunch` and `devserver.py`.**
Copying a `.xex` over FTP and launching it from Aurora takes a minute, and you do it after every change. The
launcher cuts that to a single command on the PC, and it brings the logs back:

1. Build the launcher with `run dev/scripts/build-devlaunch.sh` and copy `build/devlaunch/dist/devlaunch.xex` to
   the console. Next to it, put a `devlaunch.txt` that contains `<your PC's IP> 7380`.
2. On the PC, run `python3 dev/devserver.py`.
3. To run a new build on the console: `python3 dev/devserver.py --bump`. The console downloads it and runs it, and
   when the program exits, its logs are uploaded to `build/devserver/logs/`.

Other targets are `--set-target decbench`, `hybench` and `devlaunch`; the last one updates the launcher itself.
With `--args "autotest seconds=120"`, the app connects, streams for that long and reports how it went, with no one
holding the controller. The launcher also makes itself DashLaunch's boot program and can reboot the console
remotely, which matters when the console's Wi-Fi goes deaf (see above) and you're in another room.

**Measuring speed: `dev/bench/`.**
`decbench` times H.264 decoding on the console in every setup from the [Performance](#performance) table. It checks
each frame against checksums made on a PC, so a faster but wrong decoder can't sneak in. `hybench` does the same
for the GPU decoder. It has a watchdog that reboots the console if the GPU hangs, so you don't have to get up.

**Checking the GPU decoder on a PC: `dev/hybrid_test/`.**
This runs the GPU decoder's maths (the same C model the shaders follow) on a PC and compares every pixel with
FFmpeg's output. It takes seconds, where a console round-trip takes minutes, so a mistake shows up before it can hang
a console. You only need it if you change the GPU decoder (`app/src/gpumc.c`,
`app/shaders/mc_*`, or the FFmpeg hook).

**Seeing the menus without a console: `dev/menu_preview/`.**
This draws the real menu code into PNGs on a PC. The screenshots in this README come from it.

**Test clips: `dev/make_clips.sh`.**
This makes the 1080p clips that the benchmarks and the PC test decode. A fixed random seed makes the clips, and
therefore the checksums, identical on every machine.

```sh
dev/make_clips.sh                  # test clips (needs ffmpeg and x264 on the PC)
run dev/scripts/build-ffmpeg-c.sh  # plain-C FFmpeg, decbench's baseline
run dev/scripts/build-bench.sh     # decbench.xex, decbench_vmx.xex, hybench.xex
dev/hybrid_test/build.sh dev/bench/data/*.h264
dev/menu_preview/build.sh          # -> build/preview/*.png
```

## Layout

```
app/          the client: src/ (C), shaders/ (GLSL for xost), compat/ (POSIX bits xsdk lacks)
scripts/      the app's build scripts (run inside the toolchain container) and the shader check
patches/      changes to FFmpeg and moonlight-common-c, applied at build time
docker/       the toolchain image
docs/         screenshots
dev/          developer tools: launcher + server, benchmarks, PC tests, menu renderer, test clips
third_party/  submodules: xsdk, FFmpeg, moonlight-tv (libgamestream + moonlight-common-c), mbedtls, curl, expat, opus
```

## Credits

- [Moonlight](https://moonlight-stream.org) and [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c):
  the actual streaming protocol. We just carried it to an older house.
- [moonlight-tv](https://github.com/mariotaku/moonlight-tv): its libgamestream does the pairing and app launching.
- [xsdk](https://github.com/xenosworks/xsdk): the open Xbox 360 SDK this is built on, including its GCC 15
  toolchain and the xost shader compiler, quirks and all.
- [FFmpeg](https://ffmpeg.org), [Opus](https://opus-codec.org), mbedTLS, curl and expat.
- [Xenia](https://github.com/xenia-project/xenia): its texture-tiling code, ported in `dev/hybrid_test/tiled.c`.
- [Free60](https://free60.org) and libxenon, for documenting the hardware, the SMC sensors included.

## License

GPLv3, the same as moonlight-common-c and moonlight-tv; see [LICENSE](LICENSE). The submodules keep their own
licenses.

This project isn't affiliated with Microsoft, Moonlight, LizardByte or anyone who would be reasonably concerned
about it. Xbox and Xbox 360 are trademarks of Microsoft.
