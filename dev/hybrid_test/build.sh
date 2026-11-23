#!/bin/sh
# Builds FFmpeg for the Mac from the patched tree (once) and runs the hybrid decoder check on each clip given.
set -e
cd "$(dirname "$0")/../.."
git -C third_party/ffmpeg apply --reverse --check ../../patches/ffmpeg-xenon-vmx128.patch 2>/dev/null ||
    git -C third_party/ffmpeg apply ../../patches/ffmpeg-xenon-vmx128.patch
if [ ! -f build/ffmpeg-host/config.h ]; then
    mkdir -p build/ffmpeg-host
    (cd build/ffmpeg-host && ../../third_party/ffmpeg/configure --prefix="$PWD/../prefix-host" --disable-everything \
        --disable-autodetect --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-avfilter \
        --disable-swscale --disable-swresample --disable-network --enable-pthreads --enable-static --disable-shared \
        --disable-asm --enable-decoder=h264 --enable-parser=h264 --disable-debug >/dev/null)
fi
make -C build/ffmpeg-host -j10 >/dev/null && make -C build/ffmpeg-host install >/dev/null
cc -std=c11 -O2 -Wall -Ibuild/prefix-host/include -Ithird_party/ffmpeg -Ibuild/ffmpeg-host -Idev/bench/source \
   dev/hybrid_test/hybrid_test.c dev/hybrid_test/tiled.c dev/bench/source/mc_ref.c build/prefix-host/lib/libavcodec.a \
   build/prefix-host/lib/libavutil.a -lpthread -lm -o build/hybrid_test
for clip in "$@"; do echo "== $clip"; build/hybrid_test "$clip" 6; done
