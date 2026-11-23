#!/bin/bash
# Cross-compiles a minimal FFmpeg (H.264 decoder + parser, pthreads) for xsdk.
# Run: docker run --rm -v "$PWD":/work moonlight-xsdk-toolchain /work/scripts/build-ffmpeg-xsdk.sh
set -euo pipefail
XSDK=/work/third_party/xsdk
TC=/opt/xsdk-gcc/bin/powerpc-xsdk-elf-
mkdir -p /work/build/ffmpeg-xsdk && cd /work/build/ffmpeg-xsdk
if [ ! -f config.h ]; then
/work/third_party/ffmpeg/configure \
  --prefix=/work/build/prefix-xsdk \
  --enable-cross-compile --cross-prefix="$TC" --arch=ppc --cpu=cell --target-os=none \
  --disable-everything --disable-autodetect --disable-programs --disable-doc \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
  --disable-network --enable-pthreads --disable-w32threads --disable-os2threads \
  --disable-debug --disable-runtime-cpudetect --enable-static --disable-shared \
  --disable-altivec --disable-vsx --disable-power8 \
  --enable-decoder=h264 --enable-parser=h264 \
  --extra-cflags="-mcpu=cell -fno-pic -O3 -I$XSDK/include" \
  --extra-ldflags="-static -nostdlib -T $XSDK/src/xsdk.ld" \
  --extra-libs="-Wl,--start-group -lstdc++ $XSDK/build/libxsdk.a -lc -lm -lgcc -Wl,--end-group" \
  || { tail -60 ffbuild/config.log; exit 1; }
fi
grep -E 'define (HAVE_PTHREADS|HAVE_THREADS|HAVE_ALTIVEC|HAVE_BIGENDIAN) ' config.h
make -j"$(nproc)"
make install
