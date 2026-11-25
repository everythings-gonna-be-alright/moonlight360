#!/bin/sh
# Makes the benchmark's test clips in dev/bench/data (decbench, dev/vmx_test): 60 frames of 1080p30, a
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
clip() {   # name, entropy (cabac/cavlc), kbit/s, [slices (6)], [encoder settings (veryfast, High profile)]
    flag=""
    [ "$2" = cavlc ] && flag=--no-cabac
    # shellcheck disable=SC2086
    x264 --quiet --no-progress ${5:---preset veryfast --profile high} --tune zerolatency --bframes 0 --ref 1 \
        --slices "${4:-6}" --threads 1 \
        --bitrate "$3" --vbv-maxrate "$3" --vbv-bufsize $(($3 / 30)) --keyint infinite --no-scenecut --fps 30 \
        $flag -o "dev/bench/data/$1.h264" "$SRC"
    echo "dev/bench/data/$1.h264"
}
clip clip1080_cabac_s6 cabac 25000
clip clip1080_cabac_s6_12500 cabac 12500
clip clip1080_cavlc_s6 cavlc 25000
clip clip1080_cavlc_s6_12500 cavlc 12500
clip clip1080_cavlc_s6_6000 cavlc 6000

# For decbench's "cheaper stream" passes (AltiVec build only):
# - the 12.5 Mbit/s stream cut into 24 slices, for threads taking slices from a queue (AVCodecContext.slices);
# - "lean" streams, which leave the console less to do: no in-loop deblocking and whole-pixel motion vectors.
#   lean is x264's ultrafast preset, what Sunshine's software encoder sends when its preset is "ultrafast";
#   nvlean is the stream above with only those two things changed, as a hardware encoder could be set to send.
clip clip1080_cavlc_s24_12500 cavlc 12500 24
clip clip1080_lean_s6_12500 cavlc 12500 6 "--preset ultrafast"
clip clip1080_lean_s24_12500 cavlc 12500 24 "--preset ultrafast"
clip clip1080_lean_s24 cavlc 25000 24 "--preset ultrafast"
clip clip1080_nvlean_s24_12500 cavlc 12500 24 "--preset veryfast --profile high --no-deblock --subme 0"
# - "mid": x264's superfast preset with its fastdecode tune (no deblocking, no weighted prediction, CAVLC), what
#   Sunshine's software encoder sends with sw_preset = superfast and sw_tune = fastdecode,zerolatency. Unlike the lean
#   streams it has sub-pixel motion and smaller blocks. For decbench's tune passes.
clip clip1080_mid_s24_12500 cavlc 12500 24 "--preset superfast --no-deblock --weightp 0"
clip clip1080_mid_s24 cavlc 25000 24 "--preset superfast --no-deblock --weightp 0"

# MPEG-2, as Sunshine's software encoder sends it (mpeg2_mode = 2): Main profile, P frames, a one-second buffer. Every
# macroblock row is a slice of its own. For decbench's MPEG-2 passes.
mpeg2() {   # name, kbit/s
    ffmpeg -v error -y -i "$SRC" -c:v mpeg2video -profile:v main -bf 0 -g 600 -keyint_min 1 -sc_threshold 1000000000 \
        -flags +cgop+low_delay -b:v "$2k" -minrate "$2k" -maxrate "$2k" -bufsize "$2k" -threads 6 -thread_type slice \
        -f mpeg2video "dev/bench/data/$1.m2v"
    echo "dev/bench/data/$1.m2v"
}
mpeg2 clip1080_mpeg2_5000 5000
mpeg2 clip1080_mpeg2_12500 12500
mpeg2 clip1080_mpeg2 25000

