#!/usr/bin/env python3
"""Renders Ubuntu Sans (dev/fonts, Ubuntu Font Licence 1.0) into app/src/font_data.h: anti-aliased glyphs in one atlas,
4 bits a texel, for the UI's four text sizes. Characters Ubuntu Sans lacks (the triangles, U+25B2 and the like, and the
arrow U+2192) come from Inter (SIL Open Font License), at the same weight. Needs Pillow and fontTools:
python3 -m pip install pillow fonttools.

    python3 dev/fonts/gen_font.py

Each size replaces one scale of the old 10x20 bitmap font: a line is 20 x scale pixels and the baseline sits 16 x scale
below the line's top, as misc-fixed's did, so text keeps its place in the layout."""
import os
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "app", "src", "font_data.h")

ATLAS_W = 1024
PAD = 1  # empty texels around each glyph, so neighbours never bleed in

# What the UI writes: ASCII, Latin-1, Cyrillic (Ukrainian included) and the symbols the menus use.
SYMBOLS = [0x2026, 0x203A, 0x2039, 0x2022, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x20AC, 0x25B2, 0x25B6,
           0x25BC, 0x25C0, 0x2713, 0x2192, 0x21BB]
FULL = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + list(range(0x400, 0x460)) + [0x490, 0x491] + SYMBOLS
SMALL = list(range(0x20, 0x7F)) + SYMBOLS  # the big sizes: titles and PIN digits only

# scale, weight, pixel size, characters. Sizes 3 and 4 fall back to size 2 (stretched) for anything else. The pixel
# sizes give Ubuntu Sans's capitals (0.693 em) about the old font's 13 pixels a scale. The last two, 0.8 and 0.7, are
# scale 1 drawn smaller (the stats HUD's sizes, gfx_scale_begin): their own glyphs, so they stay as sharp.
SIZES = [
    (1, "Medium", 19, FULL),
    (2, "SemiBold", 37, FULL),
    (3, "SemiBold", 56, SMALL),
    (4, "SemiBold", 75, SMALL),
    (0.8, "Medium", 15, FULL),
    (0.7, "Medium", 13, FULL),
]

# The menus' icons (icons.h's Icon, in its order): Material Symbols Rounded names, from dev/fonts/
# MaterialSymbolsRounded-icons.ttf (make_icon_font.py cuts it). Each is drawn ICON_PX square, centred.
ICONS = [
    ("APP", "apps"),
    ("HOST", "desktop_windows"),
    ("RESOLUTION", "aspect_ratio"),
    ("FPS", "movie"),
    ("BITRATE", "signal_cellular_alt"),
    ("FAST", "fast_forward"),
    ("LATENCY", "bolt"),
    ("STATS", "monitoring"),
    ("SPEAKER", "volume_up"),
    ("WAVE", "graphic_eq"),
    ("PACKET", "package_2"),
    ("SLICES", "view_agenda"),
    ("DECODE", "smart_display"),
    ("DRAW", "brush"),
    ("LOG", "description"),
    ("THEME", "palette"),
    ("RECONNECT", "autorenew"),
    ("HUD_CORNER", "picture_in_picture_alt"),
    ("RUMBLE", "vibration"),
    ("WAKE", "power_settings_new"),
    ("TEST", "science"),
    ("KEYBOARD", "keyboard"),
]
ICON_CODES = {
    "apps": 0xE5C3, "desktop_windows": 0xE30C, "aspect_ratio": 0xE85B, "movie": 0xE404,
    "signal_cellular_alt": 0xE202, "fast_forward": 0xE01F, "bolt": 0xEA0B, "monitoring": 0xF190,
    "volume_up": 0xE050, "graphic_eq": 0xE1B8, "package_2": 0xF569, "view_agenda": 0xE8E9,
    "smart_display": 0xF06A, "brush": 0xE3AE, "description": 0xE873, "palette": 0xE40A, "autorenew": 0xE863,
    "picture_in_picture_alt": 0xE911, "vibration": 0xF2CB, "power_settings_new": 0xF8C7, "science": 0xEA4B,
    "keyboard": 0xE312,
}
ICON_PX = 30


def render(font, ch):
    """The glyph's coverage, trimmed, and where its top left sits from the pen on the baseline."""
    left, top, right, bottom = font.getbbox(ch, anchor="ls")
    w, h = right - left, bottom - top
    if w <= 0 or h <= 0:
        return None, 0, 0
    img = Image.new("L", (w + 4, h + 4), 0)
    ImageDraw.Draw(img).text((2 - left, 2 - top), ch, font=font, fill=255, anchor="ls")
    box = img.getbbox()
    if not box:
        return None, 0, 0
    img = img.crop(box)
    return img, left - 2 + box[0], top - 2 + box[1]


def main():
    glyph_tables = []
    x = y = shelf = 0
    atlas_rows = []  # (image, ax, ay)
    for scale, weight, px, chars in SIZES:
        main_path = os.path.join(HERE, f"UbuntuSans-{weight}.ttf")
        main_font = ImageFont.truetype(main_path, px)
        has = TTFont(main_path).getBestCmap()
        inter = ImageFont.truetype(os.path.join(HERE, f"Inter-{weight}.ttf"), round(px * 0.95))
        baseline = round(16 * scale)
        glyphs = []
        for point in sorted(set(chars)):   # sorted: the app looks them up by binary search
            ch = chr(point)
            font = main_font if point in has else inter
            img, left, top = render(font, ch)
            advance = font.getlength(ch)
            ax = ay = w = h = 0
            if img:
                w, h = img.size
                if x + w + PAD > ATLAS_W:
                    x, y, shelf = 0, y + shelf + PAD, 0
                ax, ay = x, y
                atlas_rows.append((img, ax, ay))
                x += w + PAD
                shelf = max(shelf, h)
            glyphs.append((point, ax, ay, w, h, left, baseline + top, round(advance * 64)))
        glyph_tables.append(glyphs)
    # The icons: each centred in an ICON_PX square, its ink's place in that square kept (left, top).
    icon_font = ImageFont.truetype(os.path.join(HERE, "MaterialSymbolsRounded-icons.ttf"), ICON_PX)
    icons = []
    for _, name in ICONS:
        canvas = Image.new("L", (ICON_PX, ICON_PX), 0)
        ImageDraw.Draw(canvas).text((ICON_PX / 2, ICON_PX / 2), chr(ICON_CODES[name]), font=icon_font, fill=255,
                                    anchor="mm")
        box = canvas.getbbox()
        img = canvas.crop(box)
        w, h = img.size
        if x + w + PAD > ATLAS_W:
            x, y, shelf = 0, y + shelf + PAD, 0
        atlas_rows.append((img, x, y))
        icons.append((len(icons), x, y, w, h, box[0], box[1], ICON_PX * 64))
        x += w + PAD
        shelf = max(shelf, h)
    atlas_h = y + shelf + PAD
    atlas_h = (atlas_h + 31) // 32 * 32
    atlas = Image.new("L", (ATLAS_W, atlas_h), 0)
    for img, ax, ay in atlas_rows:
        atlas.paste(img, (ax, ay))
    data = atlas.tobytes()
    # 4 bits a texel, rounded: 16 levels of coverage are plenty for text.
    packed = bytes(((data[i] * 15 + 127) // 255) << 4 | ((data[i + 1] * 15 + 127) // 255)
                   for i in range(0, len(data), 2))

    with open(OUT, "w") as f:
        f.write("/* Generated by dev/fonts/gen_font.py from Ubuntu Sans (Ubuntu Font Licence 1.0, dev/fonts/UbuntuSans-LICENCE.txt),\n"
                " * Inter's triangles (SIL Open Font License, dev/fonts/Inter-LICENSE.txt) and Material Symbols Rounded's icons\n"
                " * (Apache License 2.0, dev/fonts/MaterialSymbols-LICENSE.txt).\n"
                " * Do not edit. */\n#pragma once\n#include <stdint.h>\n\n")
        f.write(f"enum {{ FONT_ATLAS_WIDTH = {ATLAS_W}, FONT_ATLAS_HEIGHT = {atlas_h}, FONT_SIZES = {len(SIZES)} }};\n\n")
        f.write("typedef struct {\n    uint16_t point;\n    uint16_t x, y;        /* in the atlas */\n"
                "    uint8_t w, h;\n    int8_t left;          /* from the pen */\n"
                "    int16_t top;          /* from the line's top */\n    uint16_t advance;     /* 1/64 pixel */\n"
                "} FontGlyph;\n\n")
        for i, glyphs in enumerate(glyph_tables):
            f.write(f"/* Scale {SIZES[i][0]}, by code point. */\nstatic const FontGlyph font_glyphs_{i + 1}[] = {{\n")
            for g in glyphs:
                f.write("    {%d, %d, %d, %d, %d, %d, %d, %d},\n" % g)
            f.write("};\n\n")
        f.write("static const struct {\n    const FontGlyph *glyphs;\n    int glyph_count;\n} font_sizes[FONT_SIZES] = {\n")
        for i, glyphs in enumerate(glyph_tables):
            f.write(f"    {{ font_glyphs_{i + 1}, {len(glyphs)} }},\n")
        f.write("};\n\n")
        f.write(f"/* The icons, in icons.h's order: left and top place each in its {ICON_PX}-pixel square. */\n")
        f.write(f"enum {{ FONT_ICONS = {len(icons)}, FONT_ICON_PX = {ICON_PX} }};\n")
        f.write("static const FontGlyph font_icons[FONT_ICONS] = {\n")
        for g in icons:
            f.write("    {%d, %d, %d, %d, %d, %d, %d, %d},\n" % g)
        f.write("};\n\n/* Two texels a byte, the left one in the high nibble. */\n")
        f.write("static const uint8_t font_atlas[FONT_ATLAS_WIDTH * FONT_ATLAS_HEIGHT / 2] = {\n")
        for i in range(0, len(packed), 32):
            f.write("    " + ",".join(str(b) for b in packed[i:i + 32]) + ",\n")
        f.write("};\n")
    print(f"{OUT}: atlas {ATLAS_W}x{atlas_h}, {sum(len(g) for g in glyph_tables)} glyphs", file=sys.stderr)


if __name__ == "__main__":
    main()
