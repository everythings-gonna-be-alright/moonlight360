#!/bin/bash
# Builds libxsdk.a (the SDK itself) inside the moonlight-xsdk-toolchain container.
# Run: docker run --rm -v "$PWD":/work moonlight-xsdk-toolchain /work/scripts/build-xsdk.sh
set -euo pipefail
cd /work/third_party/xsdk
make TOOLCHAIN=/opt/xsdk-gcc/bin/powerpc-xsdk-elf- -j"$(nproc)" build/libxsdk.a
