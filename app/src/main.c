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
static void stats_line(char *out, size_t size, uint32_t shown_frames)
{
    static uint64_t last_ms;
    static uint32_t last_decoded, last_shown;
    static char line[260] = "";
    uint64_t now = now_ms();
    if (now - last_ms >= 1000) {
        VideoStats v;
        AudioStats a;
        video_get_stats(&v);
        audio_get_stats(&a);
        float seconds = (now - last_ms) / 1000.0f;
        snprintf(line, sizeof line,
                 "decode %.0f fps (%.1f ms)  shown %.0f fps  dropped %u  errors %u  audio gaps %u  %s%s",
                 (v.decoded - last_decoded) / seconds, v.decode_ms_avg, (shown_frames - last_shown) / seconds,
                 (unsigned)v.dropped, (unsigned)v.errors, (unsigned)a.silent_frames,
                 v.cabac < 0 ? "" : v.cabac ? "CABAC" : "CAVLC", v.fast_decode ? "  fast decode" : "");
        if (v.gpu_decode) {
            const size_t n = strlen(line);
            snprintf(line + n, sizeof line - n, "  GPU decode %.1f + replay %.1f ms", v.gpu_ms, v.replay_ms);
        }
        /* Every 5 s into the log too, so a freeze shows up there. */
        static unsigned logged;
        if (++logged % 5 == 0) {
            printf("[I] stats: %s\n", line);
            diag_log("streaming");
            netfix_log_datagrams();
        }
        last_ms = now;
        last_decoded = v.decoded;
        last_shown = shown_frames;
    }
    snprintf(out, size, "%s", line);
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
            AVFrame *frame = video_take_frame();
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

        if (streaming) {
            AVFrame *frame = video_take_frame();
            if (frame && !config.draw) {
                av_frame_free(&frame);   /* test switch: decoded, never copied to the GPU */
                shown_frames++;
            } else if (frame) {
                render_set_frame(frame);
                shown_frames++;
            }
        } else if (was_streaming) {
            render_clear();
        }
        was_streaming = streaming;

        uint32_t address;
        if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS))) {
            printf("[E] xsdk_present_begin timed out\n");
            break;
        }
        gpudec_service();   /* GPU decoding's work for the decode thread, before this frame's drawing */
        xsdk_gpu_set_target(width, height);
        gfx_frame_begin(width, height);
        perf_gpu_frame_begin();
        if (streaming) {
            if (!render_draw(width, height))
                ui_draw_status(width, height, "Starting the stream", "Waiting for the first frame from the PC\xE2\x80\xA6",
                               NULL, true);
            char line[260];
            stats_line(line, sizeof line, shown_frames);
            if (config.show_stats) {
                PerfReadings perf;
                perf_get(&perf);
                ui_draw_stats(line, &perf);
            }
            overlay_draw(width, height, config.app);
        } else if (in_session) {
            draw_status(width, height, &view);
        } else {
            menu_draw(width, height);
        }
        xsdk_gpu_resolve(address, width, height, CLEAR_ARGB);
        perf_gpu_frame_end();
        xsdk_present_end();
        render_frame_submitted();
        gfx_frame_end();

        if (streaming && overlay_is_open()) {
            switch (overlay_update(&input)) {
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
