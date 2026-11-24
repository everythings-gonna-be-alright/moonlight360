#!/bin/sh
# Whole-decoder check of the Xenon vector code without a console: builds the patched FFmpeg for PowerPC Linux with
# AltiVec (once), decodes each clip given under QEMU, and compares every frame's pixels with the PC's own decode
# (FFmpeg in plain C, built here into build/prefix-host). With deblocking and without; 1 thread and 6.
set -e
cd "$(dirname "$0")/../.."
git -C third_party/ffmpeg apply --reverse --check ../../patches/ffmpeg-xenon-vmx128.patch 2>/dev/null ||
    git -C third_party/ffmpeg apply ../../patches/ffmpeg-xenon-vmx128.patch
# The PC's FFmpeg: the same patched tree, without any assembly. Configured again if an older build lacks MPEG-2.
grep -qs 'define CONFIG_MPEG2VIDEO_DECODER 1' build/ffmpeg-host/config_components.h || rm -f build/ffmpeg-host/config.h
if [ ! -f build/ffmpeg-host/config.h ]; then
    mkdir -p build/ffmpeg-host build/vmx_test
    (cd build/ffmpeg-host && ../../third_party/ffmpeg/configure --prefix="$PWD/../prefix-host" --disable-everything \
        --disable-autodetect --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-avfilter \
        --disable-swscale --disable-swresample --disable-network --enable-pthreads --enable-static --disable-shared \
        --disable-asm --enable-decoder=h264 --enable-parser=h264 --enable-decoder=mpeg2video --enable-parser=mpegvideo \
        --disable-debug >/dev/null)
fi
make -C build/ffmpeg-host -j8 >/dev/null && make -C build/ffmpeg-host install >/dev/null
cc -std=c11 -O2 -Ibuild/prefix-host/include dev/vmx_test/decode_check.c build/prefix-host/lib/libavcodec.a \
   build/prefix-host/lib/libavutil.a -lpthread -lm -o build/vmx_test/decode_check_pc
docker image inspect moonlight-vmx-test >/dev/null 2>&1 || docker build -q -t moonlight-vmx-test dev/vmx_test
docker run --rm -v "$PWD":/work -w /work moonlight-vmx-test sh -c '
  set -e
  mkdir -p build/ffmpeg-ppclinux && cd build/ffmpeg-ppclinux
  grep -qs "define CONFIG_MPEG2VIDEO_DECODER 1" config_components.h || rm -f config.h
  [ -f config.h ] || ../../third_party/ffmpeg/configure --prefix=/work/build/prefix-ppclinux \
      --enable-cross-compile --cross-prefix=powerpc-linux-gnu- --arch=ppc --cpu=g4 --target-os=linux \
      --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-avdevice \
      --disable-avformat --disable-avfilter --disable-swscale --disable-swresample --disable-network \
      --enable-pthreads --disable-debug --disable-runtime-cpudetect --enable-static --disable-shared \
      --enable-altivec --disable-vsx --disable-power8 --enable-decoder=h264 --enable-parser=h264 \
      --enable-decoder=mpeg2video --enable-parser=mpegvideo >/dev/null
  make -j"$(nproc)" >/dev/null 2>&1 && make install >/dev/null 2>&1
  cd /work
  powerpc-linux-gnu-gcc -std=gnu11 -O2 -static -Ibuild/prefix-ppclinux/include dev/vmx_test/decode_check.c \
      build/prefix-ppclinux/lib/libavcodec.a build/prefix-ppclinux/lib/libavutil.a -lpthread -lm \
      -o build/vmx_test/decode_check_ppc'
fail=0
for clip in "$@"; do
    for skip in 0 1; do
        want=$(build/vmx_test/decode_check_pc "$clip" 1 $skip)
        for threads in 1 6; do
            got=$(docker run --rm -v "$PWD":/work -w /work moonlight-vmx-test \
                  qemu-ppc -cpu 7400 build/vmx_test/decode_check_ppc "$clip" $threads $skip)
            if [ "$got" = "$want" ]; then verdict=same; else verdict="DIFFERENT (PC: $want)"; fail=1; fi
            echo "$(basename "$clip"), deblocking $([ $skip = 1 ] && echo skipped || echo on), $threads thread(s): $got: $verdict"
        done
    done
done
[ $fail = 0 ] && echo "PASS: the PowerPC decode with the Xenon vector code matches the PC's" || { echo FAIL; exit 1; }
