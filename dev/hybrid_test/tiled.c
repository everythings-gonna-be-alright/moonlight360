/* Xenos's 2D tiled layout on the Mac, for checking the hybrid decoder's tiled residual: xsdk's src/tiled.c
 * (xsdk_tiled_2d), which comes from xenia:src/xenia/gpu/texture_address.h:174-238.
 *
 * Xenia carries this notice: Copyright (c) 2015, Ben Vanik. All rights reserved. Redistribution and use in source and
 * binary forms, with or without modification, are permitted provided that the following conditions are met:
 * redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer; redistributions in binary form must reproduce it in the documentation and/or other materials provided
 * with the distribution; neither the name of the project nor the names of its contributors may be used to endorse or
 * promote products derived from this software without specific prior written permission. THIS SOFTWARE IS PROVIDED
 * BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. */
#include <stdint.h>

static uint32_t combine(uint32_t outer_inner_bytes, uint32_t bank, uint32_t pipe, uint32_t y_lsb)
{
    return y_lsb << 4 | pipe << 6 | bank << 11 | (outer_inner_bytes & 0xF) | (outer_inner_bytes >> 4 & 1) << 5 |
           (outer_inner_bytes >> 5 & 7) << 8 | (outer_inner_bytes >> 8) << 12;
}

uint32_t tiled_2d(uint32_t x, uint32_t y, uint32_t pitch_texels, uint32_t bytes_per_texel_log2)
{
    const uint32_t outer_blocks = ((y >> 5) * (pitch_texels >> 5) + (x >> 5)) << 6;
    const uint32_t inner_blocks = (y >> 1 & 7) << 3 | (x & 7);
    const uint32_t bank = y >> 4 & 1;
    const uint32_t pipe = (x >> 3 & 3) ^ (y >> 3 & 1) << 1;
    return combine((outer_blocks | inner_blocks) << bytes_per_texel_log2, bank, pipe, y & 1);
}
