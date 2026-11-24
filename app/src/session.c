#include "session.h"
#include "wol.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <Limelight.h>
#include <client.h>
#include <errors.h>

#include "audio.h"
#include "diag.h"
#include "logbuf.h"
#include "netfix.h"
#include "placement.h"
#include "input.h"
#include "video.h"

#define KEY_DIR SESSION_KEY_DIR
#define DEVICE_NAME "Xbox 360"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ended_cond = PTHREAD_COND_INITIALIZER;
static SessionView view;
static Config config;
static pthread_t thread;
static bool thread_running;
static atomic_bool stop_requested;
static atomic_bool quit_requested;
static bool stream_ended;
static int termination_error;

static void set_step(SessionStep step)
{
    pthread_mutex_lock(&lock);
    view.step = step;
    pthread_mutex_unlock(&lock);
}

static void set_view(SessionState state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_view(SessionState state, const char *fmt, ...)
{
    va_list args;
    pthread_mutex_lock(&lock);
    view.state = state;
    if (state != SESSION_PAIRING)
        view.pin[0] = 0;
    va_start(args, fmt);
    vsnprintf(view.text, sizeof view.text, fmt, args);
    va_end(args);
    printf("[I] session: %s\n", view.text);
    pthread_mutex_unlock(&lock);
}

static const char *gs_message(int code)
{
    const char *message = NULL;
    gs_get_error(&message);
    if (message)
        return message;
    switch (code) {
    case GS_IO_ERROR: return "network or file error";
    case GS_UNSUPPORTED_VERSION: return "unsupported host version";
    case GS_WRONG_STATE: return "host is in the wrong state";
    default: return "error";
    }
}

/* --- moonlight-common-c connection listener -------------------------------------------------------------------- */

static void stage_starting(int stage)
{
    set_view(SESSION_WORKING, "%s...", LiGetStageName(stage));
}

static void stage_failed(int stage, int error)
{
    printf("[E] session: stage %s failed: %d\n", LiGetStageName(stage), error);
}

static void connection_terminated(int error)
{
    printf("[E] session: connection terminated, error %d (%s)\n", error,
           error == ML_ERROR_GRACEFUL_TERMINATION ? "graceful"
           : error == ML_ERROR_NO_VIDEO_TRAFFIC   ? "no video traffic"
           : error == ML_ERROR_NO_VIDEO_FRAME     ? "no video frames completed (heavy packet loss)"
           : error == ML_ERROR_UNEXPECTED_EARLY_TERMINATION ? "unexpected early termination"
           : error == ML_ERROR_PROTECTED_CONTENT  ? "protected content"
           : error == ML_ERROR_FRAME_CONVERSION   ? "frame conversion"
                                                  : "other");
    pthread_mutex_lock(&lock);
    stream_ended = true;
    termination_error = error;
    pthread_cond_broadcast(&ended_cond);
    pthread_mutex_unlock(&lock);
}

/* moonlight-common-c logs from its network threads, and each line is a synchronous write to the log file. A burst
 * (one line per failed packet) can stall a receive thread long enough for its socket to back up, so at most
 * LOG_LINES_PER_SECOND lines are written each second and the rest are counted. */
enum { LOG_LINES_PER_SECOND = 20 };

static void log_message(const char *fmt, ...)
{
    static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
    static time_t window;
    static int lines, suppressed;
    pthread_mutex_lock(&log_lock);
    time_t now = time(NULL);
    if (now != window) {
        if (suppressed)
            printf("[W] common-c: %d log lines suppressed\n", suppressed);
        window = now;
        lines = suppressed = 0;
    }
    bool write_line = lines++ < LOG_LINES_PER_SECOND;
    if (!write_line)
        suppressed++;
    pthread_mutex_unlock(&log_lock);
    if (!write_line)
        return;
    va_list args;
    va_start(args, fmt);
    printf("[I] common-c: ");
    vprintf(fmt, args);
    va_end(args);
}

/* --- The flow ---------------------------------------------------------------------------------------------------- */

static GS_CLIENT open_client(void)
{
    GS_CLIENT client = gs_new(KEY_DIR);
    if (client)
        return client;
    set_view(SESSION_WORKING, "First run: generating a client certificate.\nThis takes a little while...");
    int ret = gs_conf_init(KEY_DIR);
    if (ret != GS_OK) {
        set_view(SESSION_FAILED, "Could not create keys: %s (%d)", gs_message(ret), ret);
        return NULL;
    }
    client = gs_new(KEY_DIR);
    if (!client)
        set_view(SESSION_FAILED, "Could not load the new keys: %s", gs_message(GS_FAILED));
    return client;
}

static bool pair(GS_CLIENT client, PSERVER_DATA server)
{
    unsigned char r[2];
    getentropy(r, sizeof r);
    char pin[5];
    snprintf(pin, sizeof pin, "%04u", (unsigned)((r[0] << 8 | r[1]) % 10000));
    set_view(SESSION_PAIRING,
             "Pairing with %s\n\nOpen Sunshine's web UI on the PC, go to the PIN page, and enter:\n\n    %s\n\n"
             "Device name: " DEVICE_NAME,
             server->hostname ? server->hostname : config.host, pin);
    pthread_mutex_lock(&lock);
    snprintf(view.pin, sizeof view.pin, "%s", pin);
    pthread_mutex_unlock(&lock);
    gs_set_timeout(client, 60);
    int ret = gs_pair(client, server, pin);
    gs_set_timeout(client, 5);
    if (ret != GS_OK) {
        set_view(SESSION_FAILED, "Pairing failed: %s (%d)", gs_message(ret), ret);
        return false;
    }
    return true;
}

static int find_app(GS_CLIENT client, PSERVER_DATA server)
{
    PAPP_LIST list = NULL;
    int ret = gs_applist(client, server, &list);
    if (ret != GS_OK) {
        set_view(SESSION_FAILED, "Could not get the app list: %s (%d)", gs_message(ret), ret);
        return -1;
    }
    char names[256] = "";
    int id = -1;
    for (PAPP_LIST app = list; app; app = app->next) {
        if (id < 0 && !strcasecmp(app->name, config.app))
            id = app->id;
        if (strlen(names) + strlen(app->name) + 3 < sizeof names) {
            strcat(names, names[0] ? ", " : "");
            strcat(names, app->name);
        }
    }
    while (list) {
        PAPP_LIST next = list->next;
        free(list->name);
        free(list);
        list = next;
    }
    if (id < 0)
        set_view(SESSION_FAILED, "No app named \"%s\" on the host.\nApps: %s\nChange the App setting.", config.app,
                 names);
    return id;
}

static void *flow(void *arg)
{
    (void)arg;
    /* After a stream, XNet has been seen to stop reaching the host until the title restarted: start it afresh
     * before every connection but the first (no socket is open between sessions). */
    /* No XNet restart between sessions: it never brought a dead network back (2026-11-22), and it's one more
     * variable in the tests. netfix_restart_network() stays available. */
    static SERVER_DATA server;
    memset(&server, 0, sizeof server);

    diag_log("connect");
    set_view(SESSION_WORKING, "Loading keys...");
    GS_CLIENT client = open_client();
    if (!client)
        return NULL;
    gs_set_device_name(client, DEVICE_NAME);

    set_view(SESSION_WORKING, "Contacting %s...", config.host);
    /* "unsupported" = true: skip GameStream's legacy checks that the resolution/fps exactly match a display mode the
     * host reports, and that the host's version is a known GFE one. Sunshine scales any resolution. */
    const bool wake = config.wake_on_lan && wol_send(config.host, config.host_mac);
    int ret = gs_get_status(client, &server, config.host, 0, true);
    /* Woken, the PC takes a while to answer: ask again, waking it again each time, for up to WAKE_WAIT_S. */
    enum { WAKE_WAIT_S = 40, WAKE_EVERY_S = 4 };
    for (int waited = 0; wake && ret != GS_OK && ret != GS_UNSUPPORTED_VERSION && waited < WAKE_WAIT_S &&
                         !atomic_load(&stop_requested);
         waited += WAKE_EVERY_S) {
        set_view(SESSION_WORKING, "Waking the PC up... (%d s)", waited);
        sleep(WAKE_EVERY_S);
        wol_send(config.host, config.host_mac);
        memset(&server, 0, sizeof server);
        ret = gs_get_status(client, &server, config.host, 0, true);
    }
    if (ret == GS_OK || ret == GS_UNSUPPORTED_VERSION) {   /* its MAC, for waking it next time */
        pthread_mutex_lock(&lock);
        snprintf(view.mac, sizeof view.mac, "%s", server.mac ? server.mac : "");
        pthread_mutex_unlock(&lock);
    }
    if (ret != GS_OK && ret != GS_UNSUPPORTED_VERSION) {
        set_view(SESSION_FAILED, "Could not reach Sunshine at %s:\n%s (%d)\nCheck the Host PC setting.",
                 config.host, gs_message(ret), ret);
        goto out;
    }
    set_step(SESSION_STEP_PAIR);
    if (!server.paired && !pair(client, &server))
        goto out;
    set_step(SESSION_STEP_START_APP);
    if (atomic_load(&stop_requested))
        goto out;

    set_view(SESSION_WORKING, "Finding \"%s\"...", config.app);
    int app_id = find_app(client, &server);
    if (app_id < 0)
        goto out;
    pthread_mutex_lock(&lock);
    view.app_id = app_id;
    pthread_mutex_unlock(&lock);

    /* Sunshine resumes whatever app is running, whichever is asked for: close a different one first. */
    if (server.currentGame != 0 && server.currentGame != app_id) {
        set_view(SESSION_WORKING, "Closing the app running on the PC...");
        ret = gs_quit_app(client, &server);
        if (ret != GS_OK) {
            set_view(SESSION_FAILED, "Another app is running on the PC and could not be closed: %s (%d)",
                     gs_message(ret), ret);
            goto out;
        }
    }

    STREAM_CONFIGURATION stream;
    LiInitializeStreamConfiguration(&stream);
    stream.width = config.width;
    stream.height = config.height;
    stream.fps = config.fps;
    stream.bitrate = config.bitrate_kbps;
    stream.packetSize = config.packet_size;
    stream.streamingRemotely = STREAM_CFG_AUTO;
    stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    /* MPEG-2 where the setting asks for it and the host offers it (its session description says so; this app never
     * sees the host's codec list, libgamestream drops it); H.264 from any other host. */
    stream.supportedVideoFormats = VIDEO_FORMAT_H264 | (config.codec == CODEC_MPEG2 ? VIDEO_FORMAT_MPEG2 : 0);
    stream.clientRefreshRateX100 = 6000;
    stream.colorSpace = COLORSPACE_REC_709;
    stream.colorRange = COLOR_RANGE_LIMITED;
    /* As moonlight-tv: audio encrypted; video only if the host insists (decrypting every video packet costs CPU). */
    stream.encryptionFlags = ENCFLG_AUDIO;
    server.serverInfo.serverCodecModeSupport = SCM_H264 | (config.codec == CODEC_MPEG2 ? SCM_MPEG2 : 0);
    {
        extern int LiPreferLegacyVideoFormats;   /* patches/moonlight-common-c-legacy-codecs.patch */
        LiPreferLegacyVideoFormats = config.codec == CODEC_MPEG2;
    }

    set_view(SESSION_WORKING, "Starting \"%s\" at %dx%d %d fps, %d kbps...", config.app, stream.width, stream.height,
             stream.fps, stream.bitrate);
    printf("[I] session: packet size %d, video decode %s, draw %s, audio output %s\n", stream.packetSize,
           config.decode ? "on" : "OFF (test)", config.draw ? "on" : "OFF (test)",
           config.audio_output ? "on" : "OFF (test)");
    ret = gs_start_app(client, &server, &stream, app_id, server.isGfe, false, config.audio_on_host, 1, NULL);
    if (ret != GS_OK) {
        set_view(SESSION_FAILED, "Could not start \"%s\": %s (%d)", config.app, gs_message(ret), ret);
        goto out;
    }

    CONNECTION_LISTENER_CALLBACKS listener;
    LiInitializeConnectionCallbacks(&listener);
    listener.stageStarting = stage_starting;
    listener.stageFailed = stage_failed;
    listener.connectionTerminated = connection_terminated;
    listener.logMessage = log_message;
    if (config.input && config.rumble)
        listener.rumble = input_rumble;

    pthread_mutex_lock(&lock);
    stream_ended = false;
    termination_error = 0;
    pthread_mutex_unlock(&lock);
    input_reset();
    video_set_fast_decode(config.fast_decode);
    video_set_slices(config.slices);
    {
        /* patches/moonlight-common-c-sdp-options.patch: 5 is Moonlight's own choice, so no override then. */
        extern int LiAudioPacketDurationOverrideMs;
        LiAudioPacketDurationOverrideMs = config.audio_packet_ms == 5 ? 0 : config.audio_packet_ms;
        printf("[I] session: audio packets of %d ms asked of the host; sound %s\n", config.audio_packet_ms,
               config.audio_on_host ? "on the PC (the host still sends the audio stream)" : "on the console");
    }
    video_set_decode_enabled(config.decode);
    video_set_decode_priority(config.decode_priority);
    placement_set_default_priority(config.net_priority);
    audio_set_output_enabled(config.audio_output);

    netfix_reset_datagrams();
    set_step(SESSION_STEP_STREAM);
    ret = LiStartConnection(&server.serverInfo, &stream, &listener, &video_callbacks, &audio_callbacks, NULL, 0, NULL,
                            0);
    if (ret != 0) {
        set_view(SESSION_FAILED, "Could not connect the stream (%d).\nSee moonlight360.log.", ret);
        goto out;
    }
    {
        /* moonlight-common-c's own record of what the host agreed to encrypt (Connection.c). */
        extern uint32_t EncryptionFeaturesEnabled;
        extern bool AudioEncryptionEnabled;
        printf("[I] session: encryption: audio %s, video %s, control v2 %s\n", AudioEncryptionEnabled ? "on" : "off",
               (EncryptionFeaturesEnabled & 0x02) ? "on" : "off", (EncryptionFeaturesEnabled & 0x01) ? "on" : "off");
    }
    netfix_keepalive(config.host, config.keepalive_pps);
    set_step(SESSION_STEP_PICTURE);
    set_view(SESSION_STREAMING, "Streaming");

    pthread_mutex_lock(&lock);
    while (!stream_ended && !atomic_load(&stop_requested))
        pthread_cond_wait(&ended_cond, &lock);
    int error = termination_error;
    bool by_user = !stream_ended;
    pthread_mutex_unlock(&lock);

    set_view(SESSION_WORKING, "Disconnecting...");
    netfix_keepalive(config.host, 0);
    diag_log("stream ending");
    netfix_log_datagrams();
    LiStopConnection();
    if (config.input)
        input_stop_rumble();
    diag_log("stream stopped");
    if (by_user && atomic_load(&quit_requested)) {
        set_view(SESSION_WORKING, "Closing \"%s\" on the PC...", config.app);
        ret = gs_quit_app(client, &server);
        if (ret == GS_OK)
            set_view(SESSION_ENDED, "Closed \"%s\" on the PC.", config.app);
        else
            set_view(SESSION_ENDED, "Disconnected, but closing \"%s\" on the PC failed: %s (%d)", config.app,
                     gs_message(ret), ret);
    } else if (by_user)
        set_view(SESSION_ENDED, "Disconnected. \"%s\" is still running on the PC: Connect resumes it.",
                 config.app);
    else if (error == ML_ERROR_GRACEFUL_TERMINATION)
        set_view(SESSION_ENDED, "The host ended the stream.");
    else
        set_view(SESSION_ENDED, "Connection lost (error %d).", error);

out:
    gs_destroy(client);
    diag_log("session end");
    return NULL;
}

void session_start(const Config *c)
{
    session_stop();
    config = *c;
    atomic_store(&stop_requested, false);
    atomic_store(&quit_requested, false);
    logbuf_begin();   /* no log writes to USB storage while the session runs; main.c ends it */
    set_step(SESSION_STEP_REACH);
    pthread_mutex_lock(&lock);
    view.mac[0] = 0;
    view.app_id = 0;
    pthread_mutex_unlock(&lock);
    set_view(SESSION_WORKING, "Starting...");
    thread_running = pthread_create(&thread, NULL, flow, NULL) == 0;
    if (!thread_running)
        set_view(SESSION_FAILED, "Could not start the session thread");
}

void session_stop(void)
{
    if (!thread_running)
        return;
    pthread_mutex_lock(&lock);
    atomic_store(&stop_requested, true);
    pthread_cond_broadcast(&ended_cond);
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    thread_running = false;
}

void session_request_stop(bool quit_app)
{
    pthread_mutex_lock(&lock);
    atomic_store(&quit_requested, quit_app);
    atomic_store(&stop_requested, true);
    pthread_cond_broadcast(&ended_cond);
    pthread_mutex_unlock(&lock);
}

void session_get_view(SessionView *out)
{
    pthread_mutex_lock(&lock);
    *out = view;
    pthread_mutex_unlock(&lock);
}
