#!/bin/bash
# Builds build/devlaunch/dist/devlaunch.xex. Run: docker run --rm -v "$PWD":/work moonlight-xsdk-toolchain /work/dev/scripts/build-devlaunch.sh
set -euo pipefail
touch /work/dev/devlaunch/main.c
cmake -S /work/dev/devlaunch -B /work/build/devlaunch -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/work/third_party/xsdk/cmake/xsdk.cmake -DXSDK_TOOLCHAIN=/opt/xsdk-gcc -DCMAKE_BUILD_TYPE=Release
cmake --build /work/build/devlaunch
mkdir -p /work/build/devlaunch/dist && cp /work/build/devlaunch/devlaunch.xex /work/build/devlaunch/dist/
