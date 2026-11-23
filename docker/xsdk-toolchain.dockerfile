# xsdk cross toolchain (powerpc-xsdk-elf: GCC 15, binutils, newlib) on Debian 13.
# Build context: third_party/xsdk/tools/toolchain
FROM debian:13
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev curl ca-certificates \
      xz-utils patch python3 cmake ninja-build git pkg-config nasm \
    && rm -rf /var/lib/apt/lists/*
COPY . /opt/xsdk-toolchain-src
RUN (PREFIX=/opt/xsdk-gcc WORK=/tmp/xsdk-gcc-work bash /opt/xsdk-toolchain-src/build.sh \
      || (tail -50 /tmp/xsdk-gcc-work/logs/*.log; exit 1)) \
    && rm -rf /tmp/xsdk-gcc-work
ENV PATH=/opt/xsdk-gcc/bin:$PATH
WORKDIR /work
# Shader compiler for xsdk_add_shaders (GLSL -> SPIR-V); xost's own compiler is built per checkout.
RUN apt-get update && apt-get install -y --no-install-recommends glslang-tools && rm -rf /var/lib/apt/lists/*
