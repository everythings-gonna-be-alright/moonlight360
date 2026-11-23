#!/bin/bash
# Builds the console benchmarks: build/bench-xsdk/{decbench,decbench_vmx,hybench}.xex.
# Needs both FFmpeg builds (dev/scripts/build-ffmpeg-c.sh, scripts/build-ffmpeg.sh) and the clips (dev/make_clips.sh).
# Run: docker run --rm -v "$PWD":/work moonlight-xsdk-toolchain /work/dev/scripts/build-bench.sh
set -euo pipefail
cmake -S /work/dev/bench/xsdk -B /work/build/bench-xsdk -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/work/third_party/xsdk/cmake/xsdk.cmake -DXSDK_TOOLCHAIN=/opt/xsdk-gcc \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /work/build/bench-xsdk
# xost can drop a shader body silently, which hangs the GPU (scripts/check_shaders.py).
python3 /work/scripts/check_shaders.py /work/build/bench-xsdk/*_shaders.h
ls -la /work/build/bench-xsdk/*.xex
