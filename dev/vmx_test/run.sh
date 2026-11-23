#!/bin/sh
# Builds dev/vmx_test/vmx_test.c for big-endian PowerPC with AltiVec and runs it under QEMU (a G4), in Docker:
# the Xenon vector code against FFmpeg's C code, on a PC. Needs FFmpeg configured for the console
# (scripts/build-ffmpeg.sh), for its config.h.
set -e
cd "$(dirname "$0")/../.."
docker image inspect moonlight-vmx-test >/dev/null 2>&1 || docker build -q -t moonlight-vmx-test dev/vmx_test
docker run --rm -v "$PWD":/work -w /work moonlight-vmx-test sh -c '
  mkdir -p build/vmx_test &&
  powerpc-linux-gnu-gcc -std=gnu11 -O2 -mcpu=7400 -maltivec -mabi=altivec -static -Wall -Wno-unused-function \
      -DHAVE_AV_CONFIG_H -Ithird_party/ffmpeg -Ibuild/ffmpeg-xsdk-vmx \
      dev/vmx_test/vmx_test.c -o build/vmx_test/vmx_test &&
  bad=$(powerpc-linux-gnu-objdump -d build/vmx_test/vmx_test |
        grep -cE "\s(vmladduhm|vmule[us][bh]|vmulo[us][bh]|vmsum[a-z]+|vsum[a-z0-9]+|vmhaddshs|vmhraddshs)\s" || true) &&
  echo "instructions Xenon lacks: $bad" && [ "$bad" = 0 ] &&
  qemu-ppc -cpu 7400 build/vmx_test/vmx_test'
