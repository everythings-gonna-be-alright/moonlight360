#!/usr/bin/env python3
"""The README's animated tour, docs/screenshots/v0.3/tour.webp: the menu preview's scripted walk through the menus,
as an animated WebP (full colour, unlike a GIF's 256), 1280x720 (the console's own size) at 60 frames a second,
as the console draws the menus. Needs Pillow.

    sh dev/menu_preview/build.sh
    build/preview/menu_preview <frames folder> tour
    python3 dev/menu_preview/make_tour.py <frames folder>"""
import glob
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "docs", "screenshots", "v0.3", "tour.webp")
QUALITY = int(os.environ.get("TOUR_QUALITY", "70"))   # real game art compresses worse than the menus


def main():
    files = sorted(glob.glob(os.path.join(sys.argv[1], "tour-*.png")))
    frames = [Image.open(f).convert("RGB") for f in files]
    # 60 a second: frames of 17 and 16 ms in turn (a frame's time is whole milliseconds).
    durations = [17 if i % 3 != 2 else 16 for i in range(len(frames))]
    frames[0].save(OUT, save_all=True, append_images=frames[1:], duration=durations, loop=0, quality=QUALITY,
                   method=6, minimize_size=True, kmin=9, kmax=60)   # half the size, the same quality
    print(f"{OUT}: {len(frames)} frames, {os.path.getsize(OUT)} bytes")


if __name__ == "__main__":
    main()
