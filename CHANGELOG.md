# Changelog

## v0.4.0

- **A new look, Fresh,** the default: covers first, sections in Settings, a centred stream menu, a short startup animation. The old look stays as Classic (*Settings → Look*).
- **Latency estimate** in *Settings → Quality*: PC to TV for the settings chosen, and whether decoding keeps up.
- **Forget this PC** (*Settings → PC*, hold A).
- **MPEG-2 is the recommended codec for 1080p:** the best the console does, in performance and picture.
- **Wi-Fi:** two freeze triggers avoided (rumble, short audio packets), but a stream can still freeze. Use a cable.
- **Bitrate up to 70 Mbps.** *Fast decode* is now *Skip deblocking*.
- **Stats:** the frame's quantiser (QP) in the detailed view.
- **Faster decoding:** the frame's planes laid out to suit the CPU's cache.

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
