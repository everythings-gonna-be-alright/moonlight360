#!/bin/bash
# Cross-compiles a minimal FFmpeg (H.264 decoder + parser, pthreads) for xsdk, with AltiVec: the H.264 IDCTs, luma
# loop filter and qpel motion compensation in VMX. patches/ffmpeg-xenon-vmx128.patch takes out the vector integer
# multiplies Xenon lacks; the build fails if any are left (tools: objdump scan below).
# Run: docker run --rm -v "$PWD":/work moonlight-xsdk-toolchain /work/scripts/build-ffmpeg.sh
set -euo pipefail
cd /work/third_party/ffmpeg
if git apply --reverse --check /work/patches/ffmpeg-xenon-vmx128.patch 2>/dev/null; then :; else
  git apply /work/patches/ffmpeg-xenon-vmx128.patch && echo "applied ffmpeg-xenon-vmx128.patch"; fi
XSDK=/work/third_party/xsdk
TC=/opt/xsdk-gcc/bin/powerpc-xsdk-elf-
mkdir -p /work/build/ffmpeg-xsdk-vmx && cd /work/build/ffmpeg-xsdk-vmx
if [ ! -f config.h ]; then
/work/third_party/ffmpeg/configure \
  --prefix=/work/build/prefix-xsdk-vmx \
  --enable-cross-compile --cross-prefix="$TC" --arch=ppc --cpu=cell --target-os=none \
  --disable-everything --disable-autodetect --disable-programs --disable-doc \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
  --disable-network --enable-pthreads --disable-w32threads --disable-os2threads \
  --disable-debug --disable-runtime-cpudetect --enable-static --disable-shared \
  --enable-altivec --disable-vsx --disable-power8 \
  --enable-decoder=h264 --enable-parser=h264 \
  --extra-cflags="-mcpu=cell -fno-pic -O3 -I$XSDK/include" \
  --extra-ldflags="-static -nostdlib -T $XSDK/src/xsdk.ld" \
  --extra-libs="-Wl,--start-group -lstdc++ $XSDK/build/libxsdk.a -lc -lm -lgcc -Wl,--end-group" \
  || { tail -60 ffbuild/config.log; exit 1; }
fi
grep -E 'define (HAVE_PTHREADS|HAVE_THREADS|HAVE_ALTIVEC|HAVE_BIGENDIAN) ' config.h
make -j"$(nproc)"
make install

# No instruction Xenon's VMX128 lacks may be left in what links (docs: README "Platform notes").
bad=$(/opt/xsdk-gcc/bin/powerpc-xsdk-elf-objdump -d /work/build/prefix-xsdk-vmx/lib/libavcodec.a /work/build/prefix-xsdk-vmx/lib/libavutil.a \
  | grep -cE '\s(vmladduhm|vmule[us][bh]|vmulo[us][bh]|vmsum[a-z]+|vsum[a-z0-9]+|vmhaddshs|vmhraddshs)\s' || true)
echo "instructions Xenon lacks: $bad"
[ "$bad" = 0 ]
