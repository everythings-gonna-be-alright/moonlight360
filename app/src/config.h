/* moonlight.ini beside the .xex. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The video codec asked of the host. MPEG-2 is a Sunshine extension (its mpeg2_mode = 2); a host without it sends
 * H.264. */
enum { CODEC_H264, CODEC_MPEG2, CODECS };

/* How much the stats HUD shows while streaming. */
enum { STATS_OFF, STATS_COMPACT, STATS_DETAILED, STATS_MODES };
enum { CONFIG_RECENT = 4 };
/* Where the stats HUD sits. */
typedef enum { HUD_TOP_LEFT, HUD_TOP_RIGHT, HUD_BOTTOM_LEFT, HUD_BOTTOM_RIGHT, HUD_CORNERS } HudCorner;
/* What, held for a second while streaming, opens the stream menu (config.c's menu_combo_buttons gives the buttons). */
enum { POINTER_SPEEDS = 3 };
typedef enum { COMBO_BACK, COMBO_START, COMBO_BACK_START, COMBO_LB_RB, MENU_COMBOS } MenuCombo;
/* How big the stats HUD is: 70%, 80% and 100% of the size it's laid out at (ui.c). */
typedef enum { HUD_SMALL, HUD_MEDIUM, HUD_LARGE, HUD_SIZES } HudSize;

typedef struct {
    char host[64];      /* host PC's IP address */
    char app[64];       /* app to launch, as named in Sunshine ("Desktop") */
    int width, height;  /* stream resolution */
    int fps;
    int bitrate_kbps;
    int stats_mode;     /* STATS_OFF, STATS_COMPACT or STATS_DETAILED */
    bool fast_decode;   /* skip H.264's deblocking filter: faster, slightly blockier */
    bool low_latency;   /* draw each frame as soon as it is decoded, not on the next refresh after (main.c) */
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
    int slices;         /* slices a frame asked of the host: 6 (one a decode thread), 12, 24 or 48 (video.c) */
    int codec;          /* CODEC_H264 or CODEC_MPEG2 */
    bool ui_sounds;     /* soft clicks in the menus (uisound.c) */
    int theme;          /* the accent colour: an index into themes (theme.c), 0 orange */
    bool auto_resume;   /* on start, reconnect to the last app straight away (main.c) */
    bool show_recent;   /* the Play tab's Recent row */
    int menu_combo;     /* MenuCombo */
    int hud_corner;     /* HudCorner */
    int hud_size;       /* HudSize */
    bool rumble;        /* pass the PC's controller rumble on to the pads */
    int pointer_speed;  /* mouse mode's pointer: 0 slow, 1 normal, 2 fast (POINTER_SPEEDS, input.c) */
    /* On a 1080p output, draw a 1920x1080 picture shown pixel for pixel, where the usual 1280x720 one is scaled up by
     * the console; 1280x720 on any other output (main.c). On by default. Read once, when the app starts. */
    bool picture_1080p;
    bool wake_on_lan;   /* wake the PC with a magic packet before connecting (wol.c) */
    char host_mac[18];  /* the PC's MAC address, "aa:bb:cc:dd:ee:ff", as it last reported it; "" unknown */
    int app_id;         /* the host's id for app, for its cover; 0 unknown (a typed name) */
    /* The apps streamed lately, newest first: the host's id and the name; an empty name ends the list. */
    int recent_id[CONFIG_RECENT];
    char recent_name[CONFIG_RECENT][64];
    bool logging;       /* write the log (moonlight360.log); off by default: each line is a write to the disk */
} Config;

/* Reads the file into config (defaults for anything missing). Returns whether a host is set. */
bool config_load(const char *path, Config *config);
/* Writes the whole config back. */
bool config_save(const char *path, const Config *config);

/* Puts the app (id, name) at the front of config's recent apps, dropping it from further back and the oldest. */
void config_add_recent(Config *config, int id, const char *name);
/* Takes recent app i off the list, the ones after it moving up. */
void config_remove_recent(Config *config, int i);
/* The buttons combo needs held together (INPUT_GAMEPAD_ bits), and its name ("Back", "Back + Start"...). */
uint16_t menu_combo_buttons(int combo);
const char *menu_combo_name(int combo);
