#!/bin/sh
# Draws the app's menus into build/preview/*.png on a PC (see preview.c).
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/preview
cc -std=c11 -O2 -Wall -Wno-unused-function -D_DEFAULT_SOURCE \
   -Idev/menu_preview/stubs -Iapp/src \
   dev/menu_preview/preview.c app/src/gfx.c app/src/menu.c app/src/ui.c app/src/overlay.c \
   -lz -lm -o build/preview/menu_preview
build/preview/menu_preview build/preview
