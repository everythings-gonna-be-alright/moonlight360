/* moonlight.ini beside the .xex. */
#pragma once
#include <stdbool.h>

typedef struct {
    char host[64];      /* host PC's IP address */
    char app[64];       /* app to launch, as named in Sunshine ("Desktop") */
    int width, height;  /* stream resolution */
    int fps;
    int bitrate_kbps;
    bool show_stats;    /* overlay with frame rates and decode times */
    bool fast_decode;   /* skip H.264's deblocking filter: faster, slightly blockier */
    bool gpu_decode;    /* experimental: the GPU predicts each frame (gpudec.c); implies no deblocking */
    bool decode;        /* test switch: false receives video but never decodes it */
    bool draw;
    bool audio_output;
    int keepalive_pps;
    bool input;
    bool audio_on_host; /* play sound on the PC; Apollo still sends the audio stream (log, 2026-11-23) */
    int audio_packet_ms; /* audio packet duration asked of the host: 5 (Moonlight's default), 10 or 20 */
    int net_priority;   /* priority of Moonlight's network threads (default +2) */
    int decode_priority;         /* test switch: false never reads controllers (XamInputGetState) nor sends input */  /* Wi-Fi keepalive packets a second during a stream, 0 off */  /* test switch: false never opens the console's audio output */          /* test switch: false decodes video but never copies it to the GPU or draws it */
    int packet_size;    /* video packet size asked of the host: 1392 (Moonlight's default), 1024, 512 or 256 */
} Config;

/* Reads the file into config (defaults for anything missing). Returns whether a host is set. */
bool config_load(const char *path, Config *config);
/* Writes the whole config back. */
bool config_save(const char *path, const Config *config);
