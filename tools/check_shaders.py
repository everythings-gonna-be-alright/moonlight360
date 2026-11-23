#!/usr/bin/env python3
"""Fails if any shader in an xsdk_add_shaders header compiled to an empty program.

xost sometimes drops a shader's whole body without an error (a mat4 times a vec4 did, 2026-11-23): the program is
then 12 bytes, "alloc colors; cnop", and a pixel shader that never writes its colour hangs the GPU. Usage:
    check_shaders.py build/app/moonlight360_shaders.h [...]
"""
import re
import struct
import sys

bad = 0
for path in sys.argv[1:]:
    text = open(path).read()
    for name, body in re.findall(r"static const uint32_t (\w+)_container\[\] = \{(.*?)\};", text, re.S):
        words = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)]
        blob = struct.pack(">%dI" % len(words), *words)
        # The container header (xenos-toolchain/docs/container.md): a big-endian word at 0x08 is the ucode's size.
        size = struct.unpack_from(">I", blob, 0x08)[0] if len(blob) >= 16 else 0
        if size <= 12:
            print(f"{path}: {name} compiled to an empty program ({size} bytes)")
            bad += 1
sys.exit(1 if bad else 0)
