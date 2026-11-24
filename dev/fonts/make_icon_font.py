#!/usr/bin/env python3
"""Cuts dev/fonts/MaterialSymbolsRounded-icons.ttf, the menus' icons, out of Google's Material Symbols Rounded variable
font (Apache License 2.0, dev/fonts/MaterialSymbols-LICENSE.txt): a static instance (outlined, medium weight, drawn for
24 pixels) with only the icons gen_font.py draws. Needs fontTools.

    python3 dev/fonts/make_icon_font.py "MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf" rounded.codepoints

The variable font and its codepoints file are at github.com/google/material-design-icons, variablefont/."""
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_font import ICONS  # noqa: E402  (names, in icons.h's order)


def main():
    source, codepoints_file = sys.argv[1], sys.argv[2]
    codes = {}
    for line in open(codepoints_file):
        name, code = line.split()
        codes.setdefault(name, int(code, 16))
    wanted = [codes[name] for _, name in ICONS]
    font = instancer.instantiateVariableFont(TTFont(source), {"FILL": 0, "GRAD": 0, "opsz": 24, "wght": 500})
    options = subset.Options()
    options.layout_features = []   # no ligatures: the icons are reached by code point
    options.notdef_outline = True
    sub = subset.Subsetter(options)
    sub.populate(unicodes=wanted)
    sub.subset(font)
    out = os.path.join(HERE, "MaterialSymbolsRounded-icons.ttf")
    font.save(out)
    print(out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
