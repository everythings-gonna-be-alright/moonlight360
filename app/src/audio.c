#include "audio.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <opus_multistream.h>

#include "xsdk/audio.h"
#include "xsdk/thread.h"

enum {
    MAX_CHANNELS = 6,
    MAX_FRAME_SAMPLES = 960,          /* 20 ms at 48 kHz, the largest Opus frame Moonlight sends */
    QUEUE_SAMPLES = 48000 / 5,        /* 200 ms of samples per channel */
    MIN_PREBUFFER = 48000 * 15 / 1000,/* never start playing with less than 15 ms queued */
    FRAMES_AHEAD = 6,                 /* 32 ms queued in the kernel's driver */
    FEEDER_HW_THREAD = 1,
};

/* Moonlight's channel order (FL, FR, C, LFE, BL, BR) to xsdk's (same for these six). */
static OpusMSDecoder *decoder;
static int channels;
static float decode_buffer[MAX_FRAME_SAMPLES * MAX_CHANNELS];

/* Single-producer (audio decode thread), single-consumer (feeder) ring of interleaved samples. */
static float queue[QUEUE_SAMPLES * MAX_CHANNELS];
static atomic_uint queue_read, queue_write;   /* in sample frames, wrapping at QUEUE_SAMPLES */
static pthread_t feeder;
static atomic_bool running;
static AudioStats stats;
/* A small jitter buffer: playback starts once two packets' worth is queued, and after running dry it waits for
 * that much again instead of playing each packet the moment it lands. With 20 ms packets (needed on the console's
 * Wi-Fi) sound arrives in 20 ms lumps, and playing straight from the queue ran dry between them. */
static unsigned prebuffer;   /* samples */
static bool primed;
static bool output_enabled = true;
static bool output_open;

void audio_set_output_enabled(bool enabled)
{
    output_enabled = enabled;
}

static unsigned queued(void)
{
    return (atomic_load(&queue_write) + QUEUE_SAMPLES - atomic_load(&queue_read)) % QUEUE_SAMPLES;
}

static void *feed(void *arg)
{
    (void)arg;
    xsdk_thread_move(FEEDER_HW_THREAD);
    while (atomic_load(&running)) {
        float *out;
        NTSTATUS st = xsdk_audio_next_frame(100, &out);
        if (st == STATUS_INVALID_HANDLE)
            break;
        if (!NT_SUCCESS(st))
            continue;
        unsigned have = queued();
        if (have > prebuffer * 3) {   /* drift or a burst: drop the backlog, keep latency low */
            atomic_store(&queue_read, (atomic_load(&queue_write) + QUEUE_SAMPLES - prebuffer) % QUEUE_SAMPLES);
            have = queued();
        }
        if (!primed && have >= prebuffer)
            primed = true;
        if (primed && have < XSDK_AUDIO_FRAME_SAMPLES) {
            primed = false;           /* ran dry: a gap you'd hear; refill before playing again */
            stats.silent_frames++;
        }
        unsigned read = atomic_load(&queue_read);
        unsigned used = primed ? XSDK_AUDIO_FRAME_SAMPLES : 0;
        for (unsigned s = 0; s < XSDK_AUDIO_FRAME_SAMPLES; s++) {
            const float *in = s < used ? &queue[((read + s) % QUEUE_SAMPLES) * MAX_CHANNELS] : NULL;
            for (int c = 0; c < XSDK_AUDIO_CHANNELS; c++)
                out[c * XSDK_AUDIO_FRAME_SAMPLES + s] = (in && c < channels) ? in[c] : 0.0f;
        }
        atomic_store(&queue_read, (read + used) % QUEUE_SAMPLES);
        xsdk_audio_queue_frame();
    }
    return NULL;
}

static int init(int audio_configuration, const POPUS_MULTISTREAM_CONFIGURATION opus, void *context, int flags)
{
    int err;
    channels = opus->channelCount > MAX_CHANNELS ? MAX_CHANNELS : opus->channelCount;
    decoder = opus_multistream_decoder_create(opus->sampleRate, opus->channelCount, opus->streams,
                                              opus->coupledStreams, opus->mapping, &err);
    if (!decoder) {
        printf("[E] audio: opus decoder: %d\n", err);
        return -1;
    }
    atomic_store(&queue_read, 0);
    atomic_store(&queue_write, 0);
    memset(&stats, 0, sizeof stats);
    output_open = false;
    primed = false;
    prebuffer = (unsigned)opus->samplesPerFrame * 2;
    if (prebuffer < MIN_PREBUFFER)
        prebuffer = MIN_PREBUFFER;
    if (prebuffer > QUEUE_SAMPLES / 4)
        prebuffer = QUEUE_SAMPLES / 4;
    if (!output_enabled) {
        printf("[I] audio: %d channels, %d Hz; OUTPUT OFF (test): decoded, never played\n", opus->channelCount,
               opus->sampleRate);
        return 0;
    }
    NTSTATUS st = xsdk_audio_open(FRAMES_AHEAD);
    if (!NT_SUCCESS(st)) {
        printf("[E] audio: xsdk_audio_open: 0x%08X\n", (unsigned)st);
        opus_multistream_decoder_destroy(decoder);
        decoder = NULL;
        return -1;
    }
    atomic_store(&running, true);
    if (pthread_create(&feeder, NULL, feed, NULL) != 0) {
        xsdk_audio_close();
        return -1;
    }
    output_open = true;
    printf("[I] audio: %d channels, %d Hz, %d-sample packets, %u ms jitter buffer\n", opus->channelCount,
           opus->sampleRate, opus->samplesPerFrame, prebuffer * 1000 / 48000);
    return 0;
}

static void cleanup(void)
{
    atomic_store(&running, false);
    if (output_open) {
        xsdk_audio_close();
        pthread_join(feeder, NULL);
        output_open = false;
    }
    if (decoder)
        opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
}

static void decode_and_play(char *data, int length)
{
    int samples = opus_multistream_decode_float(decoder, (const unsigned char *)data, length, decode_buffer,
                                                MAX_FRAME_SAMPLES, 0);
    if (samples <= 0 || !output_open)
        return;
    unsigned write = atomic_load(&queue_write);
    for (int s = 0; s < samples; s++) {
        if (queued() >= QUEUE_SAMPLES - 1) {
            stats.overflows += samples - s;
            break;
        }
        memcpy(&queue[write * MAX_CHANNELS], &decode_buffer[s * channels], channels * sizeof(float));
        write = (write + 1) % QUEUE_SAMPLES;
        atomic_store(&queue_write, write);
    }
}

void audio_get_stats(AudioStats *out)
{
    *out = stats;
}

AUDIO_RENDERER_CALLBACKS audio_callbacks = {
    .init = init,
    .cleanup = cleanup,
    .decodeAndPlaySample = decode_and_play,
    .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
