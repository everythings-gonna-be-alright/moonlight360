#!/bin/bash
# Builds libxsdk.a (the SDK itself) inside the moonlight-xsdk-toolchain container.
# Run: docker run --rm -v "$PWD":/work moonlight-xsdk-toolchain /work/scripts/build-xsdk.sh
set -euo pipefail
cd /work/third_party/xsdk
# XSDK_DISPLAY_1080P: a 1920x1080 front buffer shown 1:1 on a 1080p output.
if ! git apply --reverse --check /work/patches/xsdk-display-1080p.patch 2>/dev/null; then
  git apply /work/patches/xsdk-display-1080p.patch
  echo "applied xsdk-display-1080p.patch"
fi
make TOOLCHAIN=/opt/xsdk-gcc/bin/powerpc-xsdk-elf- -j"$(nproc)" build/libxsdk.a
