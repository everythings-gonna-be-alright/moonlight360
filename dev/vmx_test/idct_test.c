/* The Xenon vector IDCT and the one-coefficient shortcut (libavcodec/ppc/mpeg_xenon_vmx.h) against FFmpeg's simple
 * IDCT in C, under QEMU (dev/vmx_test/run.sh): every pixel must match, for blocks of every kind. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/simple_idct.c"
#include "libavcodec/ppc/mpeg_xenon_vmx.h"

static uint32_t seed = 360;
static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

static long failures, checked;

static void check(const int16_t *coefficients, const char *what)
{
    static int16_t block_c[64] __attribute__((aligned(32))), block_v[64] __attribute__((aligned(32)));
    static uint8_t dest_c[8 * 32 + 32] __attribute__((aligned(16))), dest_v[8 * 32 + 32] __attribute__((aligned(16)));
    for (int add = 0; add < 2; add++)
        for (int offset = 0; offset <= 8; offset += 8) {   /* both halves of a 16-byte block */
            const int fill = rnd() % 4;
            for (unsigned i = 0; i < sizeof dest_c; i++)
                dest_c[i] = dest_v[i] = fill == 0 ? 0 : fill == 1 ? 255 : rnd();
            memcpy(block_c, coefficients, sizeof block_c);
            memcpy(block_v, coefficients, sizeof block_v);
            if (add)
                ff_simple_idct_add_int16_8bit(dest_c + offset, 32, block_c);
            else
                ff_simple_idct_put_int16_8bit(dest_c + offset, 32, block_c);
            xm_idct(dest_v + offset, 32, block_v, add);
            checked++;
            if (memcmp(dest_c, dest_v, sizeof dest_c) && failures++ < 5) {
                printf("FAIL %s (%s, offset %d):", what, add ? "add" : "put", offset);
                for (int i = 0; i < 64; i++)
                    printf(" %d", coefficients[i]);
                printf("\n");
            }
            /* The shortcuts cover blocks of a DC coefficient and perhaps the mismatch bit. */
            int only_dc = coefficients[63] == 0 || coefficients[63] == 1;
            for (int i = 1; i < 63; i++)
                only_dc &= !coefficients[i];
            if (!only_dc)
                continue;
            for (unsigned i = 0; i < sizeof dest_c; i++)
                dest_c[i] = dest_v[i] = rnd();
            memcpy(block_c, coefficients, sizeof block_c);
            if (add)
                ff_simple_idct_add_int16_8bit(dest_c + offset, 32, block_c);
            else
                ff_simple_idct_put_int16_8bit(dest_c + offset, 32, block_c);
            const int taken = add ? xm_dc_add(dest_v + offset, 32, coefficients)
                                  : xm_dc_put(dest_v + offset, 32, coefficients);
            if (!taken)   /* outside what it covers: the decoder goes on to the IDCT */
                xm_idct(dest_v + offset, 32, block_v, add);
            checked++;
            if (memcmp(dest_c, dest_v, sizeof dest_c) && failures++ < 5)
                printf("FAIL shortcut (%s), DC %d, last %d\n", add ? "add" : "put", coefficients[0], coefficients[63]);
        }
}

int main(void)
{
    int16_t b[64];
    /* Every DC value, alone and with the mismatch bit. */
    for (int dc = -2048; dc < 2048; dc++)
        for (int last = 0; last < 2; last++) {
            memset(b, 0, sizeof b);
            b[0] = dc;
            b[63] = last;
            check(b, "DC");
        }
    /* A single coefficient at each place, small and at the limits. */
    static const int16_t values[] = { 1, -1, 2, -3, 100, -100, 1023, -1024, 1024, 1025, -1025, 2047, -2048,
                                      4095, -4096, 32767, -32768 };
    for (int at = 0; at < 64; at++)
        for (unsigned v = 0; v < sizeof values / sizeof values[0]; v++)
            for (int last = 0; last < 2; last++) {
                memset(b, 0, sizeof b);
                b[63] = last;
                b[at] = values[v];
                check(b, "single");
            }
    /* Random blocks: few to all coefficients set, within MPEG-2's range, small, and anything 16 bits hold. */
    for (int round = 0; round < 120000; round++) {
        memset(b, 0, sizeof b);
        const int kind = round % 6, count = 1 + rnd() % (round % 3 ? 6 : 64);
        for (int n = 0; n < count; n++) {
            const int at = kind == 0 ? rnd() % 64 : kind == 1 ? rnd() % 8 * 8 + rnd() % 2 : rnd() % 64;
            const int v = kind == 5 ? (int)(rnd() % 65536) - 32768
                          : kind >= 3 ? (int)(rnd() % 4096) - 2048 : (int)(rnd() % 64) - 32;
            b[kind == 2 ? at % 32 : at] = v;
        }
        if (round & 1)
            b[63] ^= 1;
        check(b, "random");
    }
    /* All at the extremes. */
    for (int round = 0; round < 4000; round++) {
        for (int i = 0; i < 64; i++)
            b[i] = round < 2000 ? (rnd() & 1 ? 2047 : -2048) : (rnd() & 1 ? 32767 : -32768);
        check(b, "extremes");
    }
    printf("idct: %ld comparisons, %ld failures\n", checked, failures);
    return failures != 0;
}
