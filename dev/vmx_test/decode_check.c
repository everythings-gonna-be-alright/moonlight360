/* Decodes an H.264 clip and prints a checksum of every frame's pixels: run on the PC and, built for PowerPC with the
 * Xenon vector code, under QEMU, the two must print the same (dev/vmx_test/decode.sh).
 * Usage: decode_check clip.h264 threads [skip-deblocking 0/1] [chroma-in-C 0/1] */
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int ff_xenon_chroma_c;

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
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *c = avcodec_alloc_context3(codec);
    c->thread_count = atoi(argv[2]);
    c->thread_type = FF_THREAD_SLICE;
    if (argc > 3 && atoi(argv[3]))
        c->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(c, codec, NULL) < 0)
        return 2;
    AVCodecParserContext *p = av_parser_init(AV_CODEC_ID_H264);
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
    return 0;
}
