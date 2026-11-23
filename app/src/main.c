/*
 * Moonlight 360: a Moonlight game-streaming client prototype for the Xbox 360 (xsdk).
 *
 * Opens on a settings menu (saved to moonlight.ini beside the .xex). Connect pairs with Sunshine (showing a PIN),
 * launches the app and streams it:
 * H.264 decoded by FFmpeg on the CPU, drawn with the GPU; Opus audio; controllers 1-4 sent to the host.
 * Hold Back + Start for a second while streaming for the stream menu (resume, main menu, quit the app on the PC). The log is moonlight360.log beside the .xex.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>

#include "audio.h"
#include "devreturn.h"
#include "diag.h"
#include "gfx.h"
#include "gpudec.h"
#include "perf.h"
#include "config.h"
#include "input.h"
#include "logbuf.h"
#include "menu.h"
#include "netfix.h"
#include "overlay.h"
#include "render.h"
#include "session.h"
#include "ui.h"
#include "video.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/net.h"
#include "xsdk/present.h"
#include "xsdk/text.h"
#include "xsdk/thread.h"

#define CONFIG_PATH "moonlight.ini"
#define TITLE "Moonlight 360"

enum {
    BUFFERS = 2,
    SWAP_INTERVAL = 1,
    BEGIN_TIMEOUT_MS = 5000,

    /* Held for MENU_HOLD_MS while streaming: opens the stream menu. */
    MENU_COMBO = INPUT_GAMEPAD_BACK | INPUT_GAMEPAD_START,
    MENU_HOLD_MS = 1000,
};

/* The menus' background (theme.h THEME_BG): each resolve clears the target to it for the next frame, so the background
 * needs no draw at all. */
static const uint32_t CLEAR_ARGB = 0xFF0C1016;

/* XAM's network starts with 32 TCP sockets, 8 UDP and 16 KB buffers. Every pairing/launch request and every RTSP
 * message opens a new TCP connection, and closed ones can linger, so a second connection attempt could run out;
 * the stream's UDP sockets want bigger buffers too. XAM takes up to 232 sockets of each kind and 63 KB buffers
 * (xsdk docs/GUIDE.md, "Sockets"). xsdk reads this before main. */
const XsdkNetworkLimits xsdk_network_limits = {
    .stream_sockets = 200,
    .datagram_sockets = 32,
    .receive_kb = 63,
    .send_kb = 63,
};

/* newlib syscall xsdk does not provide; FFmpeg pulls it in through clock(). */
#include <sys/times.h>
clock_t times(struct tms *buf)
{
    (void)buf;
    return (clock_t)-1;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000;
}

static bool any_pressed(const InputState *in, uint16_t buttons)
{
    for (int i = 0; i < INPUT_PADS; i++)
        if (in->pressed[i] & buttons)
            return true;
    return false;
}

/* Stats line, refreshed once a second. */
/* Lost frames and arrivals from video.c, the ping from moonlight-common-c (its ENet estimate). */
static void net_stats(NetStats *net)
{
    video_get_net_stats(net);
    uint32_t rtt, variance;
    if (LiGetEstimatedRttInfo(&rtt, &variance)) {
        net->rtt_ms = (int)rtt;
        net->rtt_var_ms = (int)variance;
    } else {
        net->rtt_ms = net->rtt_var_ms = -1;
    }
}

/* Display latency: from a frame's decoding to its flip, which is when xsdk_present_begin next returns (with two front
 * buffers it waits for the frame queued last to show). On LiGetMicroseconds' clock, like video.c's times. */
static uint64_t queued_decoded_us;   /* the frame queued last, if it was a new one; 0 if not */
static struct {
    uint64_t start_us;
    double sum_ms;
    uint32_t frames;
    float avg_ms;   /* last second's; -1 before there is one */
} display = { .avg_ms = -1.0f };

static void display_flipped(void)
{
    const uint64_t now = LiGetMicroseconds();
    if (queued_decoded_us) {
        display.sum_ms += (double)(now - queued_decoded_us) / 1000.0;
        display.frames++;
        queued_decoded_us = 0;
    }
    if (now - display.start_us >= 1000000) {
        display.avg_ms = display.frames ? (float)(display.sum_ms / display.frames) : display.avg_ms;
        display.start_us = now;
        display.sum_ms = 0;
        display.frames = 0;
    }
}

/* Low-latency display: when the next refresh comes, so the drawing loop can wait for a new frame only while there is
 * still time to draw it before then. xsdk counts refreshes (XsdkPresentStats.vblanks) but doesn't time them, so they
 * are timed here: xsdk_present_begin returns shortly after a refresh, the period is the long-run average, and the
 * earliest-seen time of a count (counts times the period off) is when refreshes happen. Works for any TV mode.
 * The time kept for drawing (reserve) grows by a millisecond whenever a frame missed the refresh it was drawn for, and
 * shrinks very slowly while none do. */
static struct {
    bool started;
    uint64_t t0_us;           /* first sample */
    uint32_t v0;
    double period_us;
    double phase_us;          /* time of refresh v0, best estimate */
    double reserve_us;
    uint32_t target_vblank;   /* the refresh the frame queued last was drawn for; 0 if none */
    uint32_t frames, misses;  /* frames drawn the low-latency way, and those that missed their refresh */
} refresh;

static void refresh_reset(void)
{
    memset(&refresh, 0, sizeof refresh);
    refresh.period_us = 1000000.0 / 60.0;
    refresh.reserve_us = 6000.0;
}

/* Right after xsdk_present_begin: learns the refresh timing; returns how long until the next refresh, in us. */
static double refresh_sample(uint32_t *vblank_now)
{
    XsdkPresentStats st;
    xsdk_present_stats(&st);
    const uint64_t now = LiGetMicroseconds();
    *vblank_now = st.vblanks;
    if (!refresh.started) {
        refresh.started = true;
        refresh.t0_us = now;
        refresh.v0 = st.vblanks;
        refresh.phase_us = (double)now;
    }
    const uint32_t counted = st.vblanks - refresh.v0;
    if (counted >= 120)   /* two seconds at 60 Hz before trusting the average */
        refresh.period_us = (double)(now - refresh.t0_us) / counted;
    /* This refresh began at or before now: the earliest such estimate is the closest. Let it drift up by 10 us a
     * sample, so a too-early estimate (a period off at first) doesn't stick. */
    const double began = (double)now - counted * refresh.period_us;
    refresh.phase_us = began < refresh.phase_us ? began : refresh.phase_us + 10.0;
    const double next = refresh.phase_us + (counted + 1) * refresh.period_us;
    return next > (double)now ? next - (double)now : 0.0;
}

/* After xsdk_present_begin: was the frame queued last on the refresh it was drawn for? */
static void refresh_check(uint32_t last_flip_vblank)
{
    if (!refresh.target_vblank)
        return;
    refresh.frames++;
    if (last_flip_vblank > refresh.target_vblank) {
        refresh.misses++;
        refresh.reserve_us = refresh.reserve_us + 1000.0 < 12000.0 ? refresh.reserve_us + 1000.0 : 12000.0;
    } else {
        /* 1 ms up per miss, 5 us down per hit: settles where about one frame in 200 misses. */
        refresh.reserve_us = refresh.reserve_us - 5.0 > 3000.0 ? refresh.reserve_us - 5.0 : 3000.0;
    }
    refresh.target_vblank = 0;
}

static char stats_log_text[400];
static bool stats_log_due;
static struct {
    uint64_t sum_us, max_us;
    uint32_t frames;
} draw_time;   /* low-latency display: frame taken to frame queued, since the last stats_log */

/* The 5-second log lines and diagnostics, if due: after xsdk_present_end, off the frame's critical path. */
static void stats_log(void)
{
    if (!stats_log_due)
        return;
    stats_log_due = false;
    fputs(stats_log_text, stdout);
    if (refresh.frames) {
        printf("[I] stats: low-latency display: refresh %.3f ms, drawing reserve %.1f ms, drawing %.1f ms average "
               "%.1f ms longest, %u of %u frames missed their refresh\n", refresh.period_us / 1000.0,
               refresh.reserve_us / 1000.0, draw_time.frames ? draw_time.sum_us / 1000.0 / draw_time.frames : 0.0,
               draw_time.max_us / 1000.0, (unsigned)refresh.misses, (unsigned)refresh.frames);
        memset(&draw_time, 0, sizeof draw_time);
    }
    diag_log("streaming");
    netfix_log_datagrams();
}

/* The stats HUD's figures, refreshed once a second; and every 5 s the log lines (written by stats_log). */
static void stats_update(HudStats *hud, uint32_t shown_frames, const Config *config)
{
    static uint64_t last_ms;
    static uint32_t last_decoded, last_shown, last_dropped, last_errors, last_gaps;
    /* Trouble per second for the last five, to show only what is recent. */
    enum { TROUBLE_SECONDS = 5 };
    static uint32_t trouble[TROUBLE_SECONDS][3];
    static unsigned second;
    uint64_t now = now_ms();
    if (now - last_ms < 1000)
        return;
    VideoStats v;
    AudioStats a;
    NetStats net;
    video_get_stats(&v);
    audio_get_stats(&a);
    net_stats(&net);
    if (v.decoded < last_decoded || shown_frames < last_shown) {   /* a new stream: counters started over */
        last_decoded = last_shown = last_dropped = last_errors = last_gaps = 0;
        memset(trouble, 0, sizeof trouble);
    }
    const float seconds = (now - last_ms) / 1000.0f;
    const float decoded_fps = (v.decoded - last_decoded) / seconds, shown_fps = (shown_frames - last_shown) / seconds;
    const bool first = last_ms == 0;
    second++;
    uint32_t *slot = trouble[second % TROUBLE_SECONDS];
    slot[0] = v.dropped - last_dropped;
    slot[1] = v.errors - last_errors;
    slot[2] = a.silent_frames - last_gaps;

    /* PC (the host's own figure) + network (half the ping, plus first to last packet) + decode (queue included)
     * + display (to the flip). */
    const float net_ms = (net.rtt_ms >= 0 ? net.rtt_ms / 2.0f : 0.0f) + v.receive_ms;
    *hud = (HudStats){
        .ready = !first && v.decoded > 0,
        .fps = shown_fps,
        .target_fps = (float)config->fps,
        .width = config->width,
        .height = config->height,
        .cabac = v.cabac,
        .fast_decode = v.fast_decode,
        .gpu_decode = v.gpu_decode,
        .gpu_ms = v.gpu_ms,
        .replay_ms = v.replay_ms,
        .host_ms = v.host_ms,
        .net_ms = net_ms,
        .decode_ms = v.decode_latency_ms,
        .display_ms = display.avg_ms,
    };
    for (int i = 0; i < TROUBLE_SECONDS; i++) {
        hud->dropped += trouble[i][0];
        hud->errors += trouble[i][1];
        hud->audio_gaps += trouble[i][2];
    }

    /* Every 5 s into the log too, so a freeze shows up there; written by stats_log after the frame is queued,
     * since the diagnostics (heap walk, XAM's link query) take long enough to make a frame miss its refresh. */
    static unsigned logged;
    if (++logged % 5 == 0) {
        char latency[160];
        if (hud->display_ms < 0)
            snprintf(latency, sizeof latency, "latency: measuring");
        else
            snprintf(latency, sizeof latency, "latency %.0f%s ms = PC %.0f + net %.0f + decode %.0f + display %.0f",
                     (v.host_ms > 0 ? v.host_ms : 0) + net_ms + v.decode_latency_ms + display.avg_ms,
                     v.host_ms < 0 ? "+" : "", v.host_ms, net_ms, v.decode_latency_ms, display.avg_ms);
        snprintf(stats_log_text, sizeof stats_log_text,
                 "[I] stats: decode %.0f fps (%.1f ms)  shown %.0f fps  dropped %u  errors %u  audio gaps %u  %s%s%s"
                 "  ping %d ms (+-%d)  loss %.1f%% (%u frames)\n[I] stats: %s\n",
                 decoded_fps, v.decode_ms_avg, shown_fps, (unsigned)v.dropped, (unsigned)v.errors,
                 (unsigned)a.silent_frames, v.cabac < 0 ? "" : v.cabac ? "CABAC" : "CAVLC",
                 v.fast_decode ? "  fast decode" : "", v.gpu_decode ? "  GPU decoding" : "", net.rtt_ms,
                 net.rtt_var_ms, net.loss_pct, (unsigned)net.lost_frames, latency);
        stats_log_due = true;
    }
    last_ms = now;
    last_decoded = v.decoded;
    last_shown = shown_frames;
    last_dropped = v.dropped;
    last_errors = v.errors;
    last_gaps = a.silent_frames;
}

/* Automatic test mode: devlaunch passes "autotest seconds=N draw=0/1 decode=0/1 tiled=0/1 packet=N bitrate=N
 * width=N height=N fps=N fast=0/1". The app connects by itself, streams N seconds, stops, logs
 * "AUTOTEST RESULT ..." and exits back to the launcher. Settings are overridden for the run, not saved. */
typedef struct {
    bool on;
    int seconds;
    bool tiled;
} Autotest;

static int arg_int(const char *args, const char *key, int fallback)
{
    char pattern[32];
    snprintf(pattern, sizeof pattern, "%s=", key);
    const char *p = strstr(args, pattern);
    return p ? atoi(p + strlen(pattern)) : fallback;
}

static Autotest parse_autotest(Config *c)
{
    Autotest t = { .on = false, .seconds = 120, .tiled = true };
    const char *args = devreturn_args();
    if (!strstr(args, "autotest"))
        return t;
    t.on = true;
    t.seconds = arg_int(args, "seconds", 120);
    t.tiled = arg_int(args, "tiled", 1) != 0;
    c->draw = arg_int(args, "draw", c->draw) != 0;
    c->decode = arg_int(args, "decode", c->decode) != 0;
    c->fast_decode = arg_int(args, "fast", c->fast_decode) != 0;
    c->audio_output = arg_int(args, "audio", 1) != 0;
    c->keepalive_pps = arg_int(args, "keepalive", c->keepalive_pps);
    c->input = arg_int(args, "input", 1) != 0;
    c->audio_packet_ms = arg_int(args, "audiodur", c->audio_packet_ms);
    c->audio_on_host = arg_int(args, "hostaudio", c->audio_on_host) != 0;
    netfix_set_nonblocking_waits(arg_int(args, "nonblock", 0) != 0);
    logbuf_set_enabled(arg_int(args, "logbuf", 1) != 0);
    printf("[I] AUTOTEST: log during the stream %s\n",
           arg_int(args, "logbuf", 1) ? "kept in memory" : "written to the file line by line (old behaviour)");
    c->net_priority = arg_int(args, "netprio", c->net_priority);
    c->decode_priority = arg_int(args, "decprio", c->decode_priority);
    printf("[I] AUTOTEST: network threads at priority %d, decode threads at %d\n", c->net_priority,
           c->decode_priority);
    printf("[I] AUTOTEST: controller input %s\n", c->input ? "on" : "OFF (test)");
    const char *host = strstr(args, "host=");
    if (host) {
        /* e.g. host=192.168.1.20: another host for this test run */
        sscanf(host + 5, "%63s", c->host);
    }
    c->packet_size = arg_int(args, "packet", c->packet_size);
    c->bitrate_kbps = arg_int(args, "bitrate", c->bitrate_kbps);
    c->width = arg_int(args, "width", c->width);
    c->height = arg_int(args, "height", c->height);
    c->fps = arg_int(args, "fps", c->fps);
    printf("[I] AUTOTEST: host %s\n", c->host);
    printf("[I] AUTOTEST: \"%s\" -> %d s, %dx%d %d fps %d kbps, packet %d, decode %d, draw %d, audio output %d, %s textures\n", args,
           t.seconds, c->width, c->height, c->fps, c->bitrate_kbps, c->packet_size, c->decode, c->draw,
           c->audio_output, t.tiled ? "tiled" : "linear");
    return t;
}

static void draw_status(uint32_t width, uint32_t height, const SessionView *view)
{
    if (view->state == SESSION_PAIRING && view->pin[0]) {
        ui_draw_status(width, height, "Pair with your PC",
                       "Open Sunshine's web UI on the PC, go to the PIN page and enter this code. "
                       "Name the device Xbox 360. This is needed once.",
                       view->pin, true);
        return;
    }
    const bool busy = view->state == SESSION_WORKING || view->state == SESSION_IDLE;
    ui_draw_status(width, height, busy ? "Connecting" : "Stopped", view->text, NULL, busy);
}

/* Headless automatic test ("autotest ui=0 ..."): Moonlight's session with no GPU, display, text or controller
 * calls at all, and nothing logged while the stream runs. The baseline to add the app's parts back onto. */
static int run_headless(void)
{
    static Config config;
    config_load(CONFIG_PATH, &config);
    Autotest test = parse_autotest(&config);
    printf("[I] AUTOTEST: HEADLESS (no GPU, display, text or controller calls)\n");
    enum { ATTEMPTS = 5, RETRY_MS = 10000, TICK_MS = 100 };
    for (int attempt = 1; attempt <= ATTEMPTS; attempt++) {
        session_start(&config);
        uint64_t stream_start = 0;
        bool stopped = false;
        SessionView view;
        for (;;) {
            usleep(TICK_MS * 1000);
            session_get_view(&view);
            if (view.state == SESSION_FAILED || view.state == SESSION_ENDED)
                break;
            if (view.state != SESSION_STREAMING || stopped)
                continue;
            if (!stream_start)
                stream_start = now_ms();
            /* A timeline of what each socket has received, once a second (kept in memory until the end). */
            static uint64_t last_line;
            if (now_ms() - last_line >= 1000) {
                char counts[160];
                netfix_snapshot(counts, sizeof counts);
                printf("[I] t+%4.1f s received: %s\n", (now_ms() - stream_start) / 1000.0, counts);
                last_line = now_ms();
            }
            AVFrame *frame = video_take_frame(NULL);
            if (frame)
                av_frame_free(&frame);
            if (now_ms() - stream_start >= (uint64_t)test.seconds * 1000) {
                stopped = true;
                session_request_stop(false);
            }
        }
        unsigned streamed = stream_start ? (unsigned)((now_ms() - stream_start) / 1000) : 0;
        session_stop();
        logbuf_end();
        VideoStats v;
        video_get_stats(&v);
        netfix_log_datagrams();
        printf("[I] AUTOTEST: %u video frames received/decoded, %u decode errors\n", (unsigned)v.decoded,
               (unsigned)v.errors);
        if (stopped) {
            printf("[I] AUTOTEST RESULT: SURVIVED %u s (%s)\n", streamed, view.text);
            break;
        }
        if (stream_start) {
            printf("[I] AUTOTEST RESULT: DIED after %u s of streaming (%s)\n", streamed, view.text);
            break;
        }
        printf("[I] AUTOTEST: attempt %d got no stream (%s)\n", attempt, view.text);
        if (attempt == ATTEMPTS)
            printf("[I] AUTOTEST RESULT: NO STREAM after %d attempts\n", ATTEMPTS);
        else
            usleep(RETRY_MS * 1000);
    }
    diag_log("headless end");
    printf("[I] exit\n");
    devreturn();
    return 0;
}

int main(void)
{
    printf("[I] " TITLE " starting (build " __DATE__ " " __TIME__ ")\n");
    /* Above the decode workers that share core 0 with it (video.c), so drawing and input never wait a turn. */
    xsdk_thread_set_priority(XSDK_CURRENT_THREAD, 1, NULL);
    if (strstr(devreturn_args(), "autotest") && strstr(devreturn_args(), "ui=0"))
        return run_headless();
    if (!NT_SUCCESS(xsdk_gpu_open()) || !NT_SUCCESS(xsdk_text_open())) {
        printf("[E] GPU or text setup failed\n");
        return 1;
    }
    uint32_t width = 0, height = 0;
    if (!NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height))) {
        printf("[E] display setup failed\n");
        return 1;
    }
    if (!render_init() || !gfx_init())
        return 1;
    perf_start();
    printf("[I] display %ux%u\n", (unsigned)width, (unsigned)height);
    diag_log("start");

    static Config config;
    if (!config_load(CONFIG_PATH, &config))
        menu_set_message("Set the Host PC to the IP address of the PC running Sunshine, then Connect.");
    menu_open(&config, CONFIG_PATH);
    bool in_session = false;          /* a session was started and hasn't come back to the menu yet */
    Autotest autotest = parse_autotest(&config);
    render_set_tiled(autotest.tiled);
    uint64_t autotest_stream_start = 0, autotest_retry_at = 0;
    bool autotest_stopped = false;
    int autotest_attempts = 1;
    enum { AUTOTEST_ATTEMPTS = 5, AUTOTEST_RETRY_MS = 10000 };
    if (autotest.on) {
        session_start(&config);
        in_session = true;
    }

    uint32_t shown_frames = 0;
    uint64_t combo_held_since = 0;
    bool stopping = false;            /* a stop was asked for; the flow is winding down */
    bool stopped_by_user = false;     /* through the stream menu */
    bool was_streaming = false;
    bool running = true;
    while (running) {
        InputState input;
        if (config.input)
            input_poll(&input);
        else
            memset(&input, 0, sizeof input);   /* test switch: no XamInputGetState at all */
        /* An automatic retry starts before the session's state is read, so the new session isn't mistaken for the
         * failed one (that once stopped a working stream ten seconds in). */
        if (autotest_retry_at && now_ms() >= autotest_retry_at) {
            autotest_retry_at = 0;
            autotest_attempts++;
            menu_set_message(NULL);
            session_start(&config);
            in_session = true;
        }
        SessionView view;
        session_get_view(&view);
        bool streaming = in_session && view.state == SESSION_STREAMING && !stopping;

        if (autotest.on && streaming && !autotest_stream_start)
            autotest_stream_start = now_ms();
        if (autotest.on && streaming && !autotest_stopped &&
            now_ms() - autotest_stream_start >= (uint64_t)autotest.seconds * 1000) {
            printf("[I] AUTOTEST: %d s reached, stopping the stream\n", autotest.seconds);
            autotest_stopped = true;
            session_request_stop(false);
            stopping = true;
        }
        /* A failed or ended session returns to the menu, saying why. */
        if (in_session && (view.state == SESSION_FAILED || view.state == SESSION_ENDED)) {
            logbuf_end();   /* the stream is over: the log kept in memory goes to the file now */
            if (autotest.on) {
                unsigned streamed = autotest_stream_start ? (unsigned)((now_ms() - autotest_stream_start) / 1000) : 0;
                if (autotest_stopped)
                    printf("[I] AUTOTEST RESULT: SURVIVED %u s (%s)\n", streamed, view.text);
                else if (autotest_stream_start && stopped_by_user)
                    printf("[I] AUTOTEST RESULT: STOPPED BY THE USER after %u s (%s)\n", streamed, view.text);
                else if (autotest_stream_start)
                    printf("[I] AUTOTEST RESULT: DIED after %u s of streaming (%s)\n", streamed, view.text);
                else if (autotest_attempts < AUTOTEST_ATTEMPTS) {
                    /* Sunshine refuses a new session while it still holds one the console dropped: try again. */
                    printf("[I] AUTOTEST: attempt %d got no stream (%s); retrying in %d s\n", autotest_attempts,
                           view.text, AUTOTEST_RETRY_MS / 1000);
                    autotest_retry_at = now_ms() + AUTOTEST_RETRY_MS;
                } else
                    printf("[I] AUTOTEST RESULT: NO STREAM after %d attempts (%s)\n", autotest_attempts, view.text);
                if (autotest_stopped || autotest_stream_start || autotest_attempts >= AUTOTEST_ATTEMPTS)
                    running = false;
            }
            menu_set_message(view.text);
            in_session = false;
            stopping = false;
            overlay_close();
        }

        /* Normally the newest frame is taken now, before waiting for the refresh, and shows on the one after: up to
         * two refreshes after it was decoded. With low latency it is taken after the wait, waiting a little more for
         * one if none is ready, so a frame decoded in that time shows on the next refresh. */
        const bool late_take = streaming && config.low_latency && config.draw;
        uint64_t new_frame_us = 0, taken_us = 0;
        if (streaming && !late_take) {
            uint64_t decoded_us;
            AVFrame *frame = video_take_frame(&decoded_us);
            if (frame && !config.draw) {
                av_frame_free(&frame);   /* test switch: decoded, never copied to the GPU */
                shown_frames++;
            } else if (frame) {
                render_set_frame(frame);
                new_frame_us = decoded_us;
                shown_frames++;
            }
        } else if (!streaming && was_streaming) {
            render_clear();
        }
        if (streaming && !was_streaming) {   /* a new stream: no display latency yet */
            queued_decoded_us = 0;
            display.avg_ms = -1.0f;
            refresh_reset();
            printf("[I] stream settings: %dx%d@%d %d kbps, fast decode %d, GPU decoding %d, low-latency display %d, "
                   "stats %d\n", config.width, config.height, config.fps, config.bitrate_kbps, config.fast_decode,
                   config.gpu_decode, config.low_latency, config.stats_mode);
        }
        was_streaming = streaming;

        uint32_t address;
        if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS))) {
            printf("[E] xsdk_present_begin timed out\n");
            break;
        }
        if (streaming)
            display_flipped();   /* the frame queued last is on screen now */
        if (late_take) {
            XsdkPresentStats st;
            xsdk_present_stats(&st);
            refresh_check(st.last_flip_vblank);
            uint32_t vblank;
            const double until_refresh_us = refresh_sample(&vblank);
            const double wait_us = until_refresh_us - refresh.reserve_us;
            if (wait_us > 0)
                video_wait_frame((uint32_t)wait_us);
            uint64_t decoded_us;
            AVFrame *frame = video_take_frame(&decoded_us);
            if (frame) {
                render_set_frame(frame);
                new_frame_us = decoded_us;
                taken_us = LiGetMicroseconds();
                shown_frames++;
                refresh.target_vblank = vblank + 1;
            }
        }
        gpudec_service();   /* GPU decoding's work for the decode thread, before this frame's drawing */
        xsdk_gpu_set_target(width, height);
        gfx_frame_begin(width, height);
        perf_gpu_frame_begin();
        if (streaming) {
            if (!render_draw(width, height))
                ui_draw_status(width, height, "Starting the stream", "Waiting for the first frame from the PC\xE2\x80\xA6",
                               NULL, true);
            static HudStats hud;
            stats_update(&hud, shown_frames, &config);
            if (config.stats_mode != STATS_OFF) {
                PerfReadings perf;
                perf_get(&perf);
                static NetStats net;   /* 90 arrivals: off the stack */
                net_stats(&net);
                ui_draw_stats(config.stats_mode, &hud, &perf, &net);
            }
            overlay_draw(width, height, config.app, config.stats_mode);
        } else if (in_session) {
            draw_status(width, height, &view);
        } else {
            menu_draw(width, height);
        }
        xsdk_gpu_resolve(address, width, height, CLEAR_ARGB);
        perf_gpu_frame_end();
        xsdk_present_end();
        queued_decoded_us = new_frame_us;
        if (taken_us) {
            const uint64_t took = LiGetMicroseconds() - taken_us;
            draw_time.sum_us += took;
            draw_time.max_us = took > draw_time.max_us ? took : draw_time.max_us;
            draw_time.frames++;
        }
        stats_log();
        render_frame_submitted();
        gfx_frame_end();

        if (streaming && overlay_is_open()) {
            switch (overlay_update(&input, &config.stats_mode)) {
            case OVERLAY_STATS:
                config_save(CONFIG_PATH, &config);
                break;
            case OVERLAY_DISCONNECT:
                session_request_stop(false);
                stopping = true;
                stopped_by_user = true;
                break;
            case OVERLAY_QUIT_APP:
                session_request_stop(true);
                stopping = true;
                stopped_by_user = true;
                break;
            default:
                break;
            }
        } else if (streaming) {
            bool combo = false;
            for (int i = 0; i < INPUT_PADS; i++)
                combo |= (input.buttons[i] & MENU_COMBO) == MENU_COMBO;
            if (!combo) {
                combo_held_since = 0;
            } else if (!combo_held_since) {
                combo_held_since = now_ms();
            } else if (now_ms() - combo_held_since >= MENU_HOLD_MS) {
                combo_held_since = 0;
                /* The menu takes the controller: release everything on the host first. */
                input_send_neutral(&input);
                overlay_open();
                continue;
            }
            if (config.input)
                input_send(&input);
        } else if (in_session) {
            /* Connecting or pairing: the flow can't be interrupted mid-request, so Back leaves the app. */
            if (any_pressed(&input, INPUT_GAMEPAD_BACK))
                running = false;
        } else {
            switch (menu_update(&input)) {
            case MENU_CONNECT:
                menu_set_message(NULL);
                printf("[I] host %s, app \"%s\", %dx%d %d fps %d kbps\n", config.host, config.app, config.width,
                       config.height, config.fps, config.bitrate_kbps);
                session_start(&config);
                in_session = true;
                stopping = false;
                break;
            case MENU_EXIT:
                running = false;
                break;
            default:
                break;
            }
        }
    }

    /* A stream in progress is stopped; a flow still pairing or connecting simply ends with the title. */
    SessionView view;
    session_get_view(&view);
    if (view.state != SESSION_WORKING && view.state != SESSION_PAIRING)
        session_stop();
    logbuf_end();
    render_shutdown();
    gfx_shutdown();
    xsdk_present_close();
    xsdk_text_close();
    xsdk_gpu_close();
    printf("[I] exit\n");
    devreturn();
    return 0;
}
