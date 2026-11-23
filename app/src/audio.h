/* Opus decoding (moonlight-common-c's audio thread) into xsdk's 6-channel 48 kHz output (a feeder thread). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <Limelight.h>

extern AUDIO_RENDERER_CALLBACKS audio_callbacks;

typedef struct {
    uint32_t silent_frames;   /* 5.33 ms output frames played as silence (underruns) */
    uint32_t overflows;       /* decoded samples dropped because the queue was full */
} AudioStats;

void audio_get_stats(AudioStats *stats);
/* Before a stream starts: false receives and decodes audio but never opens the console's audio output (a test). */
void audio_set_output_enabled(bool enabled);
