#!/bin/sh
# Makes the benchmarks' test clips in dev/bench/data (decbench, hybench, dev/hybrid_test): 60 frames of 1080p30, a
# zooming Mandelbrot under noise (a worst case: every block has detail), encoded as Sunshine-family hosts stream:
# High profile, P frames only, 1 reference, 6 slices, CBR with a one-frame buffer. Needs ffmpeg and x264; the noise
# has a fixed seed, so the clips (and decbench's checksums) come out the same every time.
set -e
cd "$(dirname "$0")/.."
mkdir -p dev/bench/data build
SRC=build/clip_source.y4m
[ -f "$SRC" ] || ffmpeg -v error -f lavfi \
    -i "mandelbrot=s=1920x1080:r=30:start_scale=3:end_scale=0.3,noise=alls=10:allf=t+u:all_seed=360,format=yuv420p" \
    -frames:v 60 -f yuv4mpegpipe -y "$SRC"
clip() {   # name, entropy (cabac/cavlc), kbit/s
    flag=""
    [ "$2" = cavlc ] && flag=--no-cabac
    x264 --quiet --no-progress --preset veryfast --tune zerolatency --profile high --bframes 0 --ref 1 --slices 6 --threads 1 \
        --bitrate "$3" --vbv-maxrate "$3" --vbv-bufsize $(($3 / 30)) --keyint infinite --no-scenecut --fps 30 \
        $flag -o "dev/bench/data/$1.h264" "$SRC"
    echo "dev/bench/data/$1.h264"
}
clip clip1080_cabac_s6 cabac 25000
clip clip1080_cabac_s6_12500 cabac 12500
clip clip1080_cavlc_s6 cavlc 25000
clip clip1080_cavlc_s6_12500 cavlc 12500
clip clip1080_cavlc_s6_6000 cavlc 6000
