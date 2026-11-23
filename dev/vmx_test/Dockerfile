# A PowerPC (big-endian, AltiVec) cross compiler and QEMU's user-mode emulator: runs the Xenon vector code's tests
# on a PC (dev/vmx_test/run.sh).
FROM debian:13
RUN apt-get update && apt-get install -y --no-install-recommends gcc-powerpc-linux-gnu libc6-dev-powerpc-cross \
      qemu-user binutils-powerpc-linux-gnu gcc libc6-dev make && rm -rf /var/lib/apt/lists/*
