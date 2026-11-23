#!/bin/sh
# Whole-decoder check of the Xenon vector code without a console: builds the patched FFmpeg for PowerPC Linux with
# AltiVec (once), decodes each clip given under QEMU, and compares every frame's pixels with the PC's own decode
# (FFmpeg in C, build/prefix-host from dev/hybrid_test/build.sh). With deblocking and without; 1 thread and 6.
set -e
cd "$(dirname "$0")/../.."
[ -f build/prefix-host/lib/libavcodec.a ] || { echo "run dev/hybrid_test/build.sh first (the PC's FFmpeg)"; exit 1; }
make -C build/ffmpeg-host -j8 >/dev/null && make -C build/ffmpeg-host install >/dev/null
cc -std=c11 -O2 -Ibuild/prefix-host/include dev/vmx_test/decode_check.c build/prefix-host/lib/libavcodec.a \
   build/prefix-host/lib/libavutil.a -lpthread -lm -o build/vmx_test/decode_check_pc
docker image inspect moonlight-vmx-test >/dev/null 2>&1 || docker build -q -t moonlight-vmx-test dev/vmx_test
docker run --rm -v "$PWD":/work -w /work moonlight-vmx-test sh -c '
  set -e
  mkdir -p build/ffmpeg-ppclinux && cd build/ffmpeg-ppclinux
  [ -f config.h ] || ../../third_party/ffmpeg/configure --prefix=/work/build/prefix-ppclinux \
      --enable-cross-compile --cross-prefix=powerpc-linux-gnu- --arch=ppc --cpu=g4 --target-os=linux \
      --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-avdevice \
      --disable-avformat --disable-avfilter --disable-swscale --disable-swresample --disable-network \
      --enable-pthreads --disable-debug --disable-runtime-cpudetect --enable-static --disable-shared \
      --enable-altivec --disable-vsx --disable-power8 --enable-decoder=h264 --enable-parser=h264 >/dev/null
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
