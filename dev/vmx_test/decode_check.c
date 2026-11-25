/* Decodes an H.264 clip and prints a checksum of every frame's pixels: run on the PC and, built for PowerPC with the
 * Xenon vector code, under QEMU, the two must print the same (dev/vmx_test/decode.sh).
 * Usage: decode_check clip.h264|clip.m2v threads [skip-deblocking 0/1] [chroma-in-C 0/1] [tune bits]
 * A .m2v clip is MPEG-2; anything else H.264. With XENON_PROF set in the environment, an MPEG-2 clip's counts a
 * frame go to stderr: what its macroblocks and blocks are made of (libavcodec/xenon_mpeg.h). */
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int ff_xenon_chroma_c, ff_xenon_tune, ff_xenon_prof;
/* libavcodec/xenon_mpeg.h */
enum { XMPROF_PARSE, XMPROF_MC, XMPROF_IDCT, XMPROF_TOTAL, XMPROF_MID, XMPROF_MBS, XMPROF_SKIPPED, XMPROF_INTRA,
       XMPROF_NO_RESIDUAL, XMPROF_ZERO_MV, XMPROF_HPEL, XMPROF_BLOCKS = XMPROF_HPEL + 4, XMPROF_BLOCK_KIND,
       XMPROF_N = 32 };
extern uint64_t ff_xenon_mpeg_prof[128][XMPROF_N];

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *data = malloc((size_t)n + 64);
    if (fread(data, 1, (size_t)n, f) != (size_t)n)
        return 2;
    memset(data + n, 0, 64);
    ff_xenon_chroma_c = argc > 4 && atoi(argv[4]);
    if (argc > 5)
        ff_xenon_tune = atoi(argv[5]);   /* the decoder's experiments, by bit (h264dec.h); as built otherwise */
    const size_t name_len = strlen(argv[1]);
    const enum AVCodecID id = name_len > 4 && !strcmp(argv[1] + name_len - 4, ".m2v") ? AV_CODEC_ID_MPEG2VIDEO
                                                                                        : AV_CODEC_ID_H264;
    ff_xenon_prof = getenv("XENON_PROF") != NULL;
    const AVCodec *codec = avcodec_find_decoder(id);
    AVCodecContext *c = avcodec_alloc_context3(codec);
    c->thread_count = atoi(argv[2]);
    c->thread_type = FF_THREAD_SLICE;
    if (argc > 3 && atoi(argv[3]))
        c->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(c, codec, NULL) < 0)
        return 2;
    AVCodecParserContext *p = av_parser_init(id);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    const uint8_t *d = data;
    long left = n;
    uint32_t sum = 2166136261u;
    int frames = 0;
    for (int flushed = 0; !flushed;) {
        if (left > 0) {
            const int used = av_parser_parse2(p, c, &pkt->data, &pkt->size, d, (int)left, AV_NOPTS_VALUE,
                                              AV_NOPTS_VALUE, 0);
            d += used;
            left -= used;
            if (!pkt->size)
                continue;
            avcodec_send_packet(c, pkt);
        } else {
            /* The parser holds the last frame until it is told the stream is over. */
            av_parser_parse2(p, c, &pkt->data, &pkt->size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (pkt->size)
                avcodec_send_packet(c, pkt);
            avcodec_send_packet(c, NULL);
            flushed = 1;
        }
        while (avcodec_receive_frame(c, fr) >= 0) {
            for (int plane = 0; plane < 3; plane++) {
                const int w = plane ? fr->width / 2 : fr->width, h = plane ? fr->height / 2 : fr->height;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        sum = (sum ^ fr->data[plane][y * fr->linesize[plane] + x]) * 16777619u;
            }
            frames++;
        }
    }
    printf("%d frames, checksum %08x\n", frames, (unsigned)sum);
    if (ff_xenon_prof && id == AV_CODEC_ID_MPEG2VIDEO && frames) {
        double t[XMPROF_N] = { 0 };
        for (int row = 0; row < 128; row++)
            for (int k = 0; k < XMPROF_N; k++)
                t[k] += (double)ff_xenon_mpeg_prof[row][k] / frames;
        fprintf(stderr, "a frame: %.0f MBs = %.0f skipped + %.0f intra + %.0f inter (%.0f with no residual, %.0f not "
                "moved; whole-pixel %.0f, half across %.0f, half down %.0f, both %.0f)\n",
                t[XMPROF_MBS], t[XMPROF_SKIPPED], t[XMPROF_INTRA],
                t[XMPROF_MBS] - t[XMPROF_SKIPPED] - t[XMPROF_INTRA], t[XMPROF_NO_RESIDUAL] - t[XMPROF_SKIPPED],
                t[XMPROF_ZERO_MV], t[XMPROF_HPEL], t[XMPROF_HPEL + 1], t[XMPROF_HPEL + 2], t[XMPROF_HPEL + 3]);
        fprintf(stderr, "a frame: %.0f blocks = %.0f DC alone + %.0f DC and the mismatch bit + %.0f up to 3 "
                "coefficients' reach + %.0f up to 10 + %.0f up to 21 + %.0f further\n",
                t[XMPROF_BLOCKS], t[XMPROF_BLOCK_KIND], t[XMPROF_BLOCK_KIND + 1], t[XMPROF_BLOCK_KIND + 2],
                t[XMPROF_BLOCK_KIND + 3], t[XMPROF_BLOCK_KIND + 4], t[XMPROF_BLOCK_KIND + 5]);
    }
    return 0;
}
