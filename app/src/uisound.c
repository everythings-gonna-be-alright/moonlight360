#include "uisound.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/audio.h"

enum {
    RATE = XSDK_AUDIO_RATE_HZ,
    FRAMES_AHEAD = 3,          /* 16 ms queued: a click comes soon after the press */
    MAX_SAMPLES = RATE / 10,   /* the longest sound, 100 ms */
};

/* A note: frequency (Hz), start and length (ms). Each sound is one or two short sine notes that die away fast. */
typedef struct {
    float hz, start_ms, length_ms;
} Note;

static const struct {
    float volume;
    Note notes[2];
} recipes[UISOUNDS] = {
    [UISOUND_MOVE] = { 0.10f, { { 2400.0f, 0.0f, 28.0f } } },
    [UISOUND_CHANGE] = { 0.10f, { { 1700.0f, 0.0f, 36.0f } } },
    [UISOUND_SELECT] = { 0.12f, { { 880.0f, 0.0f, 45.0f }, { 1320.0f, 38.0f, 60.0f } } },
    [UISOUND_BACK] = { 0.12f, { { 1320.0f, 0.0f, 45.0f }, { 880.0f, 38.0f, 60.0f } } },
};

static float samples[UISOUNDS][MAX_SAMPLES];
static int lengths[UISOUNDS];
static bool made, open;
static pthread_t feeder;
static atomic_int request;   /* a sound to start, + 1; 0 for none */

static void make_sounds(void)
{
    for (int s = 0; s < UISOUNDS; s++) {
        for (int n = 0; n < 2 && recipes[s].notes[n].length_ms > 0.0f; n++) {
            const Note *note = &recipes[s].notes[n];
            const int first = (int)(note->start_ms * RATE / 1000.0f);
            const int count = (int)(note->length_ms * RATE / 1000.0f);
            for (int i = 0; i < count && first + i < MAX_SAMPLES; i++) {
                const float t = (float)i / RATE;
                /* 2 ms in, then dying away over about a quarter of the note: a soft tick, not a beep. */
                const float in = t < 0.002f ? t / 0.002f : 1.0f;
                const float out = expf(-t / (note->length_ms * 0.00025f));
                samples[s][first + i] += recipes[s].volume * in * out * sinf(6.2831853f * note->hz * t);
            }
            if (first + count > lengths[s])
                lengths[s] = first + count < MAX_SAMPLES ? first + count : MAX_SAMPLES;
        }
    }
    made = true;
}

/* Fills each output frame: silence, with the sound playing (if any) in the front left and right. */
static void *feed(void *arg)
{
    (void)arg;
    int playing = -1, at = 0;
    for (;;) {
        float *frame;
        const NTSTATUS st = xsdk_audio_next_frame(100, &frame);
        if (st == STATUS_INVALID_HANDLE)
            break;   /* closed */
        if (!NT_SUCCESS(st))
            continue;
        const int asked = atomic_exchange(&request, 0);
        if (asked) {
            playing = asked - 1;
            at = 0;
        }
        memset(frame, 0, XSDK_AUDIO_FRAME_FLOATS * sizeof *frame);
        if (playing >= 0) {
            float *left = frame + XSDK_AUDIO_FRONT_LEFT * XSDK_AUDIO_FRAME_SAMPLES;
            float *right = frame + XSDK_AUDIO_FRONT_RIGHT * XSDK_AUDIO_FRAME_SAMPLES;
            for (int i = 0; i < XSDK_AUDIO_FRAME_SAMPLES && at < lengths[playing]; i++, at++)
                left[i] = right[i] = samples[playing][at];
            if (at >= lengths[playing])
                playing = -1;
        }
        xsdk_audio_queue_frame();
    }
    return NULL;
}

void uisound_open(void)
{
    static bool gave_up;   /* the output wouldn't open: don't try (and log) every frame */
    if (open || gave_up)
        return;
    gave_up = true;
    if (!made)
        make_sounds();
    const NTSTATUS st = xsdk_audio_open(FRAMES_AHEAD);
    if (!NT_SUCCESS(st)) {
        printf("[W] uisound: xsdk_audio_open: 0x%08X\n", (unsigned)st);
        return;
    }
    atomic_store(&request, 0);
    if (pthread_create(&feeder, NULL, feed, NULL) != 0) {
        printf("[W] uisound: no feeder thread\n");
        xsdk_audio_close();
        return;
    }
    open = true;
    gave_up = false;
}

void uisound_close(void)
{
    if (!open)
        return;
    xsdk_audio_close();   /* the feeder's wait returns STATUS_INVALID_HANDLE */
    pthread_join(feeder, NULL);
    open = false;
}

void uisound_play(UiSound sound)
{
    if (open && (unsigned)sound < UISOUNDS)
        atomic_store(&request, (int)sound + 1);
}
