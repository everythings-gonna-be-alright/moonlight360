# Changelog

## v0.4.1

- **MPEG-2 is the recommended codec for 1080p:** the best the console does, in performance and picture. *Video codec* now shows **MPEG-2 · best quality** in green; the "experimental" warning is gone, and so are the lean-stream and Skip deblocking notices, which don't apply to it.
- **Wi-Fi, corrected:** v0.4.0 avoids the two known triggers, but a stream can still freeze on Wi-Fi after a few minutes. Use a network cable; on a cable 1080p60 at 50 Mbit/s ran 13½ minutes with nothing lost.
- README: MPEG-2 first, a shorter feature list, the Wi-Fi warning.

## v0.4.0

- **Fresh, a new look,** the default; the old one stays as Classic (Settings → Look).
  - **Play:** the game A streams, big, its cover lighting the page in its colour, or the moon and its night sky (Look → Play picture); the quality line (A opens Settings → Quality); the other recent apps under it, the page following the cursor onto them; All apps; X changes the PC.
  - **Settings:** sections at the left (Quality, PC, Controls, Sound, Look, Advanced), the rows, and the selected one explained beside them. LB and RB step through Play, Settings and About.
  - **Quality:** an estimated latency, PC to TV, for the settings chosen (PC, network, decoding, TV), and whether decoding keeps up.
  - **Connecting:** the five steps along a track that fills, the picture fading in instead of cutting in.
  - **Stream menu:** in the middle; Resume, Mouse and keyboard, Stats, Leave stream, Quit (A held for a second). B, or the button that opened it, resumes.
  - **Startup:** stars, the moon rising and turning to a crescent, the name. Any button skips it.
  - Failures say what went wrong and what to do, with Try again; with no PC yet, Find your PC.
- **Forget this PC** (Settings → PC, A held): its address, its MAC and the recent apps go.
- **Wi-Fi:** the console's Wi-Fi adapter stopped receiving until a reboot when the controllers rumbled during a stream, or with short audio packets. On Wi-Fi the app now passes no rumble to the controllers and always asks for 20 ms audio packets; it warns past 25 Mbps, about what the console's Wi-Fi carries.
- **Bitrate up to 70 Mbps** (past 50 untried: the row warns).
- **Fast decode is now Skip deblocking,** for what it does.
- **Stats:** the frame's quantiser (QP) in the detailed view.
- **Decoding:** each frame's planes laid out so the CPU's cache holds more of them.
- Audio packets in the order 5, 10, 20 ms.

## v0.3.0

- **New look:** Ubuntu Sans font, smooth Material icons, animated moon, motion, optional menu sounds.
- **Night sky:** rare events on the Play tab, minutes apart: birds, a comet, a meteor shower, a blood moon, a UFO, fireflies, a constellation, a plane, an aurora.
- **True 1080p picture:** 1920×1080 on a 1080p TV, 720p on any other; automatic.
- **Mouse and keyboard from the controller:** mouse mode, on-screen keyboard typing, Esc, Alt + Tab and other keys (stream menu: **X** mouse, **Y** type).
- **Bitrate up to 50 Mbps** (was 30), with a warning past 40 Mbps at 1080p60.
- **Themes:** Orange, Blue, Violet, Pink.
- **Find your PC:** scans the network; no typing IP addresses.
- **Covers:** app picker with box art; Recent row on the Play tab (A stream, Y choose, X remove).
- **Connecting:** step-by-step progress; plain-language errors with Try again. Start streaming answers the press with a short animation.
- **New settings:** Wake the PC (Wake-on-LAN), Reconnect on start, Rumble, Recent apps, Stats position and size.
- **MPEG-2 (beta)** codec option (Sunshine with `mpeg2_mode = 2`) and Slices per frame.
- **Stream menu:** hold Back (was Back + Start).
- **Logging** off by default; timestamped; viewable in About.
- **Settings** grouped into sections; About tab with credits.
- **Faster menus and HUD:** shapes and text drawn in batches.
- **Defaults:** 720p60, low-latency display on, fast decode off.
- Controllers polled 500×/s (was 1000).
- Removed: GPU decoding (beta).

## v0.2.0

- **Stats HUD:** compact or detailed; latency by stage, ping, loss, CPU/GPU load, graphs.
- **Low-latency display** option.
- Controllers polled ~1000×/s.

## v0.1.0

- First release: pairing, app list, H.264 up to 1080p30, Opus stereo, controllers 1–4 with rumble.
