#!/bin/bash
# Builds build/app/moonlight360.xex.
# Needs libxsdk.a, xsdk's host shader compiler and FFmpeg first:
#   scripts/build-xsdk.sh, scripts/build-ffmpeg-xsdk-vmx.sh
# Run: docker run --rm -v "$PWD":/work moonlight-xsdk-toolchain /work/scripts/build-app.sh
set -euo pipefail

# Fixes to third-party code, as patch files (patches/<name>.patch -> the checkout it applies to). Idempotent.
apply_patch() {
  local dir=$1 patch=$2
  if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then
    return   # already applied
  fi
  git -C "$dir" apply "$patch"
  echo "applied $(basename "$patch")"
}
apply_patch /work/third_party/moonlight-tv/core/moonlight-common-c \
  /work/patches/moonlight-common-c-mbedtls3-cbc-padding.patch
apply_patch /work/third_party/moonlight-tv/core/moonlight-common-c \
  /work/patches/moonlight-common-c-sdp-options.patch
make -C /work/third_party/xsdk TOOLCHAIN=/opt/xsdk-gcc/bin/powerpc-xsdk-elf- build/host/libxsc.so >/dev/null
cmake -S /work/app -B /work/build/app -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/work/third_party/xsdk/cmake/xsdk.cmake -DXSDK_TOOLCHAIN=/opt/xsdk-gcc \
  -DCMAKE_BUILD_TYPE=Release
# The build stamp (__DATE__ __TIME__) lives in these two: recompile them every build.
touch /work/app/src/main.c /work/app/src/menu.c
cmake --build /work/build/app --target moonlight360
ls -la /work/build/app/moonlight360.xex
# xost can drop a shader body silently, which hangs the GPU (tools/check_shaders.py).
python3 /work/tools/check_shaders.py /work/build/app/moonlight360_shaders.h
mkdir -p /work/build/app/dist
cp /work/build/app/moonlight360.xex /work/build/app/dist/
[ -f /work/build/app/dist/moonlight.ini ] || cp /work/app/moonlight.ini /work/build/app/dist/
ls -la /work/build/app/dist
