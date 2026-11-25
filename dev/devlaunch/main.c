/*
 * devlaunch: the console side of dev/devserver.py.
 *
 * devlaunch.txt beside the .xex: "<Mac IP> <port>", e.g. "192.168.1.20 7380". Optional further lines:
 *   noreboot            never reboot the console automatically
 *   home <path>         what Back starts instead of the dashboard, e.g. "home hdd:/Aurora/Aurora.xex" (for when
 *                       DashLaunch's Default is this launcher, so Back still gets to Aurora)
 *
 * Auto-reboot: when the server can't be reached for about half a minute (the console's network dies under some
 * tests and only a reboot brings it back), it counts down 10 s (B cancels) and reboots the console. At most twice
 * in a row without reaching the server in between (kept in devlaunch.reboots), so a Mac that is simply off doesn't
 * make it reboot for ever. To come back here after the reboot by itself, set DashLaunch's Default to this .xex.
 * On start it makes itself DashLaunch's Default (make_default below), then uploads every *.log beside it (except its own), then shows the server's current target.
 * A downloads the target's files beside it and launches it, passing this .xex's path so the program comes back here
 * when it ends (app/src/devreturn.c). Y asks the server again; Back leaves to the dashboard.
 *
 * Auto-run: it asks the server every 2 s. When the manifest's "run" number differs from the last one run (kept in
 * devlaunch.run beside the .xex), it counts down 3 s (B cancels) and runs the target by itself. The number is saved
 * before launching, so a program that crashes or comes back never runs twice for one number.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/net.h"
#include "xsdk/present.h"
#include "xsdk/text.h"
#include "xsdk/xam.h"

const XsdkNetworkLimits xsdk_network_limits = {
    .stream_sockets = 64, .datagram_sockets = 16, .receive_kb = 63, .send_kb = 63,
};

enum { MAX_FILES = 16, SCREEN_CHARS = 4096 };
/* What a slow or failed wait for the network at boot saw (wait_for_network); uploaded with the other logs. */
#define NETBOOT_LOG "netboot.log"
static const uint32_t CLEAR_ARGB = 0xFF101820, TEXT_ARGB = 0xFFFFFFFF;

static char server_ip[64];
static int server_port;
static bool auto_reboot = true;
static char home_path[160];
static uint32_t width, height;
static char screen[SCREEN_CHARS];
static bool boot_log_on;   /* say() also appends to netboot.log (wait_for_network) */

/* --- the screen ---------------------------------------------------------------------------------------------- */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    printf("%s\n", line);
    if (boot_log_on) {   /* line by line: the wait may end in a power-off or in Aurora, not in this code */
        FILE *f = fopen(NETBOOT_LOG, "a");
        if (f) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
    }
    if (strlen(screen) + strlen(line) + 2 >= sizeof screen)
        screen[0] = 0;
    strcat(screen, line);
    strcat(screen, "\n");
}

static void draw(void)
{
    uint32_t address;
    if (!NT_SUCCESS(xsdk_present_begin(&address, 5000)))
        return;
    xsdk_gpu_set_target(width, height);
    xsdk_text_draw(60, 40, 2, TEXT_ARGB, "Moonlight 360 dev launcher");
    xsdk_text_draw(60, 100, 1, TEXT_ARGB, screen);
    xsdk_gpu_resolve(address, width, height, CLEAR_ARGB);
    xsdk_present_end();
}

/* --- HTTP/1.0 over a plain socket ---------------------------------------------------------------------------------- */

static int http_open(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = { .tv_sec = 10 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(server_port) };
    inet_pton(AF_INET, server_ip, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const void *data, size_t size)
{
    const char *p = data;
    while (size) {
        ssize_t n = send(fd, p, size, 0);
        if (n <= 0)
            return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

/* Sends the request line and headers (a body, if any, follows with send_all). */
static int http_start(int fd, const char *method, const char *path, size_t body_bytes)
{
    char request[512];
    snprintf(request, sizeof request, "%s %s HTTP/1.0\r\nHost: %s\r\nContent-Length: %u\r\n\r\n", method, path,
             server_ip, (unsigned)body_bytes);
    return send_all(fd, request, strlen(request));
}

static int http_read_head(int fd, long *content_length)
{
    char head[2048];
    size_t n = 0;
    while (n + 1 < sizeof head) {
        if (recv(fd, &head[n], 1, 0) != 1)
            return -1;
        n++;
        if (n >= 4 && !memcmp(&head[n - 4], "\r\n\r\n", 4))
            break;
    }
    head[n] = 0;
    int status = 0;
    sscanf(head, "HTTP/%*s %d", &status);
    char *cl = strstr(head, "Content-Length:");
    *content_length = cl ? strtol(cl + 15, NULL, 10) : -1;
    return status;
}

/* GET into memory (out_size bytes at most, zero-terminated). Returns the status or -1. */
static int http_get_text(const char *path, char *out, size_t out_size)
{
    int fd = http_open();
    if (fd < 0)
        return -1;
    long length;
    if (http_start(fd, "GET", path, 0) != 0) {
        close(fd);
        return -1;
    }
    int status = http_read_head(fd, &length);
    size_t got = 0;
    ssize_t n;
    while (got + 1 < out_size && (n = recv(fd, out + got, out_size - 1 - got, 0)) > 0)
        got += (size_t)n;
    out[got] = 0;
    close(fd);
    return status;
}

/* GET into a file, through a temporary name. Returns bytes written or -1. */
static long http_get_file(const char *path, const char *file)
{
    int fd = http_open();
    if (fd < 0)
        return -1;
    long length;
    if (http_start(fd, "GET", path, 0) != 0 || http_read_head(fd, &length) != 200) {
        close(fd);
        return -1;
    }
    char temp[128];
    snprintf(temp, sizeof temp, "%s.part", file);
    FILE *f = fopen(temp, "wb");
    if (!f) {
        close(fd);
        return -1;
    }
    static char buf[64 * 1024];
    long total = 0;
    ssize_t n;
    while ((n = recv(fd, buf, sizeof buf, 0)) > 0) {
        fwrite(buf, 1, (size_t)n, f);
        total += n;
    }
    fclose(f);
    close(fd);
    if (length >= 0 && total != length) {
        remove(temp);
        return -1;
    }
    remove(file);
    if (rename(temp, file) != 0)
        return -1;
    return total;
}

static int http_post_file(const char *path, const char *file)
{
    FILE *f = fopen(file, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(size > 0 ? (size_t)size : 1);
    size_t got = data ? fread(data, 1, (size_t)size, f) : 0;
    fclose(f);
    int fd = data ? http_open() : -1;
    long length;
    int status = -1;
    if (fd >= 0 && http_start(fd, "POST", path, got) == 0 && send_all(fd, data, got) == 0)
        status = http_read_head(fd, &length);
    if (fd >= 0)
        close(fd);
    free(data);
    return status;
}

/* --- the launcher ------------------------------------------------------------------------------------------------ */

static void upload_logs(void)
{
    DIR *dir = opendir(".");
    if (!dir) {
        say("cannot list this folder (errno %d)", errno);
        return;
    }
    struct dirent *e;
    int uploaded = 0;
    while ((e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".log") || !strcasecmp(e->d_name, "devlaunch.log") ||
            e->d_name[0] == '.')
            continue;
        char path[300];
        snprintf(path, sizeof path, "/logs/%s", e->d_name);
        int status = http_post_file(path, e->d_name);
        say("uploaded %s: %s", e->d_name, status == 200 ? "ok" : "FAILED");
        uploaded += status == 200;
        if (status == 200 && !strcasecmp(e->d_name, NETBOOT_LOG))
            remove(NETBOOT_LOG);   /* told: the next slow start begins a new one */
    }
    closedir(dir);
    if (!uploaded)
        say("no logs to upload");
}

typedef struct {
    long run;
    char args[256];
    char target[64];
    char launch[64];
    char files[MAX_FILES][64];
    long sizes[MAX_FILES];
    int count;
} Manifest;

static int fetch_manifest(Manifest *m)
{
    static char text[4096];
    int status = http_get_text("/manifest", text, sizeof text);
    if (status != 200)
        return -1;
    memset(m, 0, sizeof *m);
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "args ", 5))
            snprintf(m->args, sizeof m->args, "%s", line + 5);
        else if (!strncmp(line, "run ", 4))
            m->run = strtol(line + 4, NULL, 10);
        else if (!strncmp(line, "target ", 7))
            snprintf(m->target, sizeof m->target, "%s", line + 7);
        else if (!strncmp(line, "launch ", 7))
            snprintf(m->launch, sizeof m->launch, "%s", line + 7);
        else if (!strncmp(line, "file ", 5) && m->count < MAX_FILES &&
                 sscanf(line + 5, "%63s %ld", m->files[m->count], &m->sizes[m->count]) == 2)
            m->count++;
    }
    return m->launch[0] ? 0 : -1;
}

static bool show_manifest(Manifest *m)
{
    say(" ");
    say("Asking %s:%d for the current build...", server_ip, server_port);
    draw();
    if (fetch_manifest(m) != 0) {
        say("No answer. Is dev/devserver.py running on the Mac?");
        say("Y: try again    Back: dashboard");
        return false;
    }
    say("Target: %s", m->target);
    for (int i = 0; i < m->count; i++)
        say("  %s  (%ld bytes)", m->files[i], m->sizes[i]);
    say("A: download and run    Y: ask again    Back: dashboard");
    return true;
}

#define RUN_STATE "devlaunch.run"

static long last_run(void)
{
    long run = -1;
    FILE *f = fopen(RUN_STATE, "r");
    if (f) {
        if (fscanf(f, "%ld", &run) != 1)
            run = -1;
        fclose(f);
    }
    return run;
}

static void save_run(long run)
{
    FILE *f = fopen(RUN_STATE, "w");
    if (f) {
        fprintf(f, "%ld\n", run);
        fclose(f);
    }
}

static void download_and_launch(const Manifest *m)
{
    save_run(m->run);   /* before anything can crash: one launch per run number */
    for (int i = 0; i < m->count; i++) {
        say("Downloading %s...", m->files[i]);
        draw();
        char path[128];
        snprintf(path, sizeof path, "/files/%s", m->files[i]);
        long got = http_get_file(path, m->files[i]);
        if (got < 0) {
            say("Download of %s failed. Y: try again", m->files[i]);
            return;
        }
    }
    char game_path[128];
    snprintf(game_path, sizeof game_path, "game:\\%s", m->launch);
    const char *self = ExLoadedImageName ? ExLoadedImageName : "";
    /* Launch data: this launcher's path (to come back to), a NUL, then the server's test arguments if any. */
    static char data[XSDK_LAUNCH_DATA_MAX_BYTES];
    size_t self_len = strlen(self) + 1, args_len = strlen(m->args) + 1;
    if (self_len + args_len > sizeof data)
        args_len = 0;
    memcpy(data, self, self_len);
    memcpy(data + self_len, m->args, args_len);
    say("Launching %s%s%s...", m->launch, m->args[0] ? " with " : "", m->args);
    draw();
    xsdk_present_close();
    xsdk_text_close();
    xsdk_gpu_close();
    fflush(stdout);
    xsdk_launch(game_path, data, self_len + args_len);
    /* Only on failure: */
    printf("launch of %s failed (errno %d)\n", game_path, errno);
    exit(1);
}

#define REBOOT_STATE "devlaunch.reboots"
enum { MAX_REBOOTS_IN_A_ROW = 2, FAILURES_BEFORE_REBOOT = 3, REBOOT_COUNTDOWN_FRAMES = 600 };

static int reboots_in_a_row(void)
{
    int n = 0;
    FILE *f = fopen(REBOOT_STATE, "r");
    if (f) {
        if (fscanf(f, "%d", &n) != 1)
            n = 0;
        fclose(f);
    }
    return n;
}

static void set_reboots_in_a_row(int n)
{
    FILE *f = fopen(REBOOT_STATE, "w");
    if (f) {
        fprintf(f, "%d\n", n);
        fclose(f);
    }
}

/* Reboots the console: xboxkrnl's HalReturnToFirmware, ordinal 0x28 (xsdk SCOPE.md, from Xenia's export table),
 * with routine 1, HalRebootRoutine (the value Xenia's xboxkrnl_hal.cc expects). Looked up at run time, as xsdk
 * has no import for it. Returns only if the lookup fails. */
static void reboot_console(void)
{
    HANDLE kernel = 0;
    void *address = NULL;
    uint32_t found = XexGetModuleHandle("xboxkrnl.exe", &kernel);
    NTSTATUS status = found == 0 ? XexGetProcedureAddress(kernel, 0x28, &address) : (NTSTATUS)found;
    printf("reboot: XexGetModuleHandle 0x%08X, XexGetProcedureAddress 0x%08X, address %p\n", (unsigned)found,
           (unsigned)status, address);
    fflush(stdout);
    if (found != 0 || !NT_SUCCESS(status) || !address)
        return;
    xsdk_present_close();
    xsdk_text_close();
    xsdk_gpu_close();
    ((void (*)(uint32_t))address)(1);
}

/* ---- the network at boot ----
 * As DashLaunch's Default this starts with the console: the link and the address can take a while to come up, and
 * talking to the Mac before then only fails (and used to count toward an automatic reboot). So it waits, showing why,
 * for up to NETWORK_WAIT_FRAMES, then goes on either way.
 *
 * On Wi-Fi the network has been seen to stay down here until Aurora was started once (2026-11-24); what Aurora does
 * for it isn't known. So while it waits the launcher starts XAM's network over again every so often, as a title that
 * starts later would, and writes what it saw to netboot.log, which goes to the Mac with the next upload. */
enum {
    NETWORK_WAIT_FRAMES = 120 * 60,
    BOOT_LOG_FROM_FRAMES = 3 * 60,   /* a wait this long is worth a netboot.log */
    POKE_FRAMES = 20 * 60,           /* a datagram sent, in case the stack waits for a first use */
    FIRST_KICK_FRAMES = 45 * 60,     /* the network started over: plain, with the "bypass security" flag, plain */
    KICK_EVERY_FRAMES = 25 * 60,
};
static bool network_waited;   /* the wait took long enough, or failed */

/* WSACleanup, XNetCleanup, XNetStartup, WSAStartup: what a title's start does (xsdk src/socket.c builds the 13-byte
 * XNetStartupParams the same way; its second byte is cfgFlags, 1 for XNET_STARTUP_BYPASS_SECURITY). */
static void restart_network(int try, uint8_t flags)
{
    const uint8_t params[13] = { [0] = 13, [1] = flags, [2] = xsdk_network_limits.datagram_sockets,
                                 [3] = xsdk_network_limits.stream_sockets, [4] = xsdk_network_limits.receive_kb,
                                 [5] = xsdk_network_limits.send_kb };
    static uint8_t wsadata[400];
    const int32_t wsa_cleanup = NetDll_WSACleanup(XNCALLER_TITLE);
    const int32_t xnet_cleanup = NetDll_XNetCleanup(XNCALLER_TITLE, NULL);
    const int32_t xnet_startup = NetDll_XNetStartup(XNCALLER_TITLE, params);
    const int32_t wsa_startup = NetDll_WSAStartup(XNCALLER_TITLE, 0x0202, wsadata);
    say("  network started again, try %d, flags %u (cleanup %d %d, startup %d %d)", try, (unsigned)flags,
        (int)wsa_cleanup, (int)xnet_cleanup, (int)xnet_startup, (int)wsa_startup);
}

/* One datagram to the server's discard port. */
static void poke_network(void)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        say("  poke: no socket (errno %d)", errno);
        return;
    }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(9) };
    inet_pton(AF_INET, server_ip, &to.sin_addr);
    const ssize_t sent = sendto(fd, "devlaunch", 9, 0, (struct sockaddr *)&to, sizeof to);
    say("  poke: a datagram to %s:9 gave %d (errno %d)", server_ip, (int)sent, sent < 0 ? errno : 0);
    close(fd);
}

static bool wait_for_network(void)
{
    say("Waiting for the network... (B: skip)");
    int last = -2, kicks = 0;
    uint32_t last_link = ~0u, last_status = ~0u;
    for (int frame = 0; frame < NETWORK_WAIT_FRAMES; frame++) {
        uint32_t link = 0;
        struct in_addr address = { 0 };
        const int link_ret = xsdk_net_link(&link);
        const int address_ret = xsdk_net_address(&address);
        /* XAM's own answer, with its bits: 0 pending, 0x04 static, 0x08 DHCP, 0x20 a gateway, 0x40 DNS... */
        XNADDR raw = { 0 };
        const uint32_t status = link_ret ? ~0u : NetDll_XNetGetTitleXnAddr(XNCALLER_TITLE, &raw);
        if (address_ret == 0 && address.s_addr && (link & XSDK_NET_LINK_ACTIVE)) {
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &address, text, sizeof text);
            say("Network up: %s, link 0x%02X (%s), after %d s", text, (unsigned)link,
                link & 0x20 ? "Wi-Fi" : "Ethernet", frame / 60);
            network_waited = frame >= BOOT_LOG_FROM_FRAMES;
            return true;
        }
        if (frame == BOOT_LOG_FROM_FRAMES) {
            /* Slow: from here every line also goes to netboot.log, after what the screen has so far. */
            FILE *f = fopen(NETBOOT_LOG, "a");
            if (f) {
                fprintf(f, "---- a start of the launcher (build " __DATE__ " " __TIME__ ") ----\n%s", screen);
                fclose(f);
            }
            boot_log_on = true;
        }
        const int now = link_ret ? -1 : !(link & XSDK_NET_LINK_ACTIVE) ? 0 : address_ret == EAGAIN ? 1 : 2;
        if (now != last || link != last_link || status != last_status) {
            say("  %2d s: %s (link 0x%02X, address status 0x%02X, start %d, address %d)", frame / 60,
                now < 0 ? "the network won't start" : now == 0 ? "no link yet (cable or Wi-Fi)"
                : now == 1 ? "link up, getting an address..." : "link up, no address",
                (unsigned)link, (unsigned)status, link_ret, address_ret);
            last = now;
            last_link = link;
            last_status = status;
        }
        if (frame == POKE_FRAMES)
            poke_network();
        if (frame >= FIRST_KICK_FRAMES && (frame - FIRST_KICK_FRAMES) % KICK_EVERY_FRAMES == 0) {
            kicks++;
            restart_network(kicks, kicks == 2 ? 1 : 0);
        }
        INPUT_STATE input = { 0 };
        if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS && (input.gamepad.buttons & INPUT_GAMEPAD_B)) {
            say("  skipped with B after %d s", frame / 60);
            break;
        }
        draw();
    }
    say("No network yet: going on; Y asks the server again.");
    network_waited = true;
    return false;
}

/* ---- DashLaunch's Default: boot straight into this launcher ----
 * DashLaunch reads launch.ini from the first drive holding one, USB before the hard disk before internal storage
 * (consolemods.org, DashLaunch). Its Default names the program the console boots into, with DashLaunch's own drive
 * names. Those names aren't xsdk's, so rather than guess one, the launcher takes it from the Default already there:
 * it finds which drive holds that file, and if this .xex is on the same drive, points Default at it under the same
 * name. The old Default becomes "home" in devlaunch.txt, which Back starts. launch.ini is kept as
 * launch.ini.devlaunch.bak first, and a copy of the result goes to the Mac as launch_ini.log. */

typedef struct {
    const char *device;   /* as the kernel names it */
} Drive;

static const Drive drives[] = {
    { "\\Device\\Mass0" }, { "\\Device\\Mass1" }, { "\\Device\\Mass2" }, { "\\Device\\Harddisk0\\Partition1" },
    { "\\Device\\BuiltInMuUsb\\Storage" }, { "\\Device\\BuiltInMuSfc" }, { "\\Device\\Mu0" }, { "\\Device\\Mu1" },
};
enum { DRIVES = sizeof drives / sizeof drives[0], INI_BYTES = 64 * 1024 };

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static void note(const char *line)
{
    FILE *f = fopen("launch_ini.log", "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    say("%s", line);
}

/* The drive of a kernel path: its index, and where the rest starts (after the backslash). */
static int drive_of(const char *path, const char **rest)
{
    for (int i = 0; i < DRIVES; i++) {
        const size_t n = strlen(drives[i].device);
        if (!strncasecmp(path, drives[i].device, n) && path[n] == '\\') {
            *rest = path + n + 1;
            return i;
        }
    }
    return -1;
}

static void make_default(void)
{
    remove("launch_ini.log");
    char line[300];
    /* This launcher as Aurora starts it: devlaunch.xex beside this .xex, also while devlaunch_new.xex installs itself. */
    char self[200];
    snprintf(self, sizeof self, "%s", ExLoadedImageName ? ExLoadedImageName : "");
    char *slash = strrchr(self, '\\');
    if (slash)
        snprintf(slash + 1, sizeof self - (size_t)(slash + 1 - self), "devlaunch.xex");
    const char *self_rest;
    const int self_drive = drive_of(self, &self_rest);
    int ini_drive = -1;
    char ini_path[200];
    for (int i = 0; i < DRIVES && ini_drive < 0; i++) {
        snprintf(ini_path, sizeof ini_path, "%s\\launch.ini", drives[i].device);
        if (file_exists(ini_path))
            ini_drive = i;
    }
    if (ini_drive < 0 || self_drive < 0) {
        snprintf(line, sizeof line, "Default: %s; not changed", ini_drive < 0 ? "no launch.ini found" : "this launcher's drive isn't known");
        note(line);
        return;
    }
    static char ini[INI_BYTES + 1];
    FILE *f = fopen(ini_path, "rb");
    const size_t size = f ? fread(ini, 1, INI_BYTES, f) : 0;
    if (f)
        fclose(f);
    ini[size] = 0;

    /* The Default line: "Default = <drive>:\<path>", under [Paths]. */
    char *at = NULL;
    for (char *p = ini; *p;) {
        char *eol = p + strcspn(p, "\r\n");
        char *k = p;
        while (*k == ' ' || *k == '\t')
            k++;
        if (!strncasecmp(k, "Default", 7) && (k[7] == ' ' || k[7] == '\t' || k[7] == '=')) {
            at = p;
            break;
        }
        p = *eol ? eol + 1 : eol;
    }
    char value[200] = "";
    if (at) {
        const char *eq = strchr(at, '=');
        const char *eol = at + strcspn(at, "\r\n");
        if (eq && eq < eol) {
            eq++;
            while (*eq == ' ' || *eq == '\t')
                eq++;
            size_t n = (size_t)(eol - eq);
            while (n && (eq[n - 1] == ' ' || eq[n - 1] == '\t'))
                n--;
            snprintf(value, sizeof value, "%.*s", (int)n, eq);
        }
    }
    snprintf(line, sizeof line, "launch.ini: %s; Default now: %s", ini_path, value[0] ? value : "(none)");
    note(line);
    if (!value[0]) {
        note("Default: no Default line to learn DashLaunch's drive name from; not changed");
        return;
    }
    const char *colon = strchr(value, ':');
    if (!colon || colon[1] != '\\') {
        note("Default: not in DashLaunch's drive:\\path form; not changed");
        return;
    }
    if (strstr(value, self_rest) && !strcasecmp(value + strlen(value) - strlen(self_rest), self_rest)) {
        note("Default: already this launcher");
        return;
    }
    /* Which drive holds the current Default: that drive is the one its name stands for. */
    const char *default_rest = colon + 2;
    int default_drive = -1;
    char probe[300];
    for (int i = 0; i < DRIVES && default_drive < 0; i++) {
        snprintf(probe, sizeof probe, "%s\\%s", drives[i].device, default_rest);
        if (file_exists(probe))
            default_drive = i;
    }
    if (default_drive < 0) {
        note("Default: its program isn't on any drive found; not changed");
        return;
    }
    if (default_drive != self_drive) {
        snprintf(line, sizeof line, "Default: its program is on %s, this launcher on %s, whose DashLaunch name isn't "
                 "known; not changed", drives[default_drive].device, drives[self_drive].device);
        note(line);
        return;
    }
    char name[32];
    snprintf(name, sizeof name, "%.*s", (int)(colon - value), value);
    char new_value[220];
    snprintf(new_value, sizeof new_value, "%s:\\%s", name, self_rest);

    /* Back up once, then write the file with only Default's value changed. */
    char backup[220];
    snprintf(backup, sizeof backup, "%s.devlaunch.bak", ini_path);
    if (!file_exists(backup)) {
        FILE *b = fopen(backup, "wb");
        if (!b || fwrite(ini, 1, size, b) != size) {
            if (b)
                fclose(b);
            note("Default: could not write the backup; not changed");
            return;
        }
        fclose(b);
    }
    const char *eq = strchr(at, '=') + 1;
    while (*eq == ' ' || *eq == '\t')
        eq++;
    const char *eol = at + strcspn(at, "\r\n");
    char temp[220];
    snprintf(temp, sizeof temp, "%s.new", ini_path);
    FILE *o = fopen(temp, "wb");
    bool ok = o && fwrite(ini, 1, (size_t)(eq - ini), o) == (size_t)(eq - ini) &&
              fputs(new_value, o) >= 0 && fwrite(eol, 1, strlen(eol), o) == strlen(eol);
    if (o)
        ok = fclose(o) == 0 && ok;
    if (!ok || (remove(ini_path) != 0) || rename(temp, ini_path) != 0) {
        note("Default: writing launch.ini failed; the backup is beside it");
        return;
    }
    snprintf(line, sizeof line, "Default: now %s (was %s)", new_value, value);
    note(line);

    /* The old Default becomes home, as a kernel path xsdk launches, unless devlaunch.txt has one. */
    snprintf(probe, sizeof probe, "%s\\%s", drives[default_drive].device, default_rest);
    if (!home_path[0]) {
        FILE *t = fopen("devlaunch.txt", "a");
        if (t) {
            fprintf(t, "\nhome %s\n", probe);
            fclose(t);
            snprintf(home_path, sizeof home_path, "%.150s", probe);
            snprintf(line, sizeof line, "home: %s (Back starts it)", probe);
            note(line);
        }
    }
}

/* Updating the launcher itself: the server's "devlaunch" target sends it as devlaunch_new.xex, which on start
 * copies itself over devlaunch.xex (not running now), so Aurora's entry starts the new one from then on. */
static void install_self_if_new(void)
{
    const char *self = ExLoadedImageName ? ExLoadedImageName : "";
    const char *slash = strrchr(self, '\\');
    if (strcasecmp(slash ? slash + 1 : self, "devlaunch_new.xex"))
        return;
    FILE *in = fopen("devlaunch_new.xex", "rb");
    FILE *out = in ? fopen("devlaunch.xex.part", "wb") : NULL;
    bool ok = in && out;
    static char buf[64 * 1024];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    if (ok) {
        remove("devlaunch.xex");
        ok = rename("devlaunch.xex.part", "devlaunch.xex") == 0;
    }
    say(ok ? "Launcher updated: devlaunch.xex is now this build." : "Launcher update FAILED (errno %d)", errno);
}

int main(void)
{
    if (!NT_SUCCESS(xsdk_gpu_open()) || !NT_SUCCESS(xsdk_text_open()) ||
        !NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, 2, 1, &width, &height)))
        return 1;
    say("build " __DATE__ " " __TIME__);
    install_self_if_new();
    FILE *f = fopen("devlaunch.txt", "r");
    char option[200];
    if (f && fscanf(f, "%63s %d", server_ip, &server_port) == 2) {
        while (fscanf(f, " %199[^\r\n]", option) == 1) {
            if (!strcmp(option, "noreboot"))
                auto_reboot = false;
            else if (!strncmp(option, "home ", 5))
                snprintf(home_path, sizeof home_path, "%.150s", option + 5);
        }
        rewind(f);
    }
    if (!f || fscanf(f, "%63s %d", server_ip, &server_port) != 2) {
        say("devlaunch.txt missing: put \"<Mac IP> <port>\" in it, e.g. \"192.168.1.20 7380\". Back: dashboard");
        if (f)
            fclose(f);
    } else {
        fclose(f);
        say("Server %s:%d", server_ip, server_port);
        wait_for_network();
        make_default();
        say("Uploading logs...");
        draw();
        upload_logs();
    }
    Manifest m;
    bool have = server_ip[0] && show_manifest(&m);
    if (boot_log_on) {
        say("  first answer from the Mac: %s", have ? "yes" : "none");
        boot_log_on = false;
    }
    if (have && last_run() < 0)
        save_run(m.run);   /* first start: don't run whatever was last bumped, only what comes next */
    if (server_ip[0])
        say("Auto-run is on: new runs from the Mac start by themselves.");
    uint16_t previous = 0;
    unsigned frame = 0;
    int countdown = -1;   /* frames left before an automatic launch; -1 none */
    int failures = have ? 0 : 1;      /* polls in a row that didn't reach the server */
    int reboot_countdown = -1;        /* frames left before an automatic reboot; -1 none */
    if (have)
        set_reboots_in_a_row(0);
    if (server_ip[0])
        say("Auto-reboot when the network dies: %s", !auto_reboot ? "off (noreboot)"
            : reboots_in_a_row() >= MAX_REBOOTS_IN_A_ROW ? "paused (rebooted twice without reaching the Mac)" : "on");
    for (;;) {
        draw();
        frame++;
        INPUT_STATE in = { 0 };
        uint16_t buttons = XamInputGetState(0, 0, &in) == ERROR_SUCCESS ? in.gamepad.buttons : 0;
        uint16_t pressed = buttons & ~previous;
        previous = buttons;
        if (pressed & INPUT_GAMEPAD_BACK)
            break;
        if ((pressed & INPUT_GAMEPAD_Y) && server_ip[0])
            have = show_manifest(&m);
        if ((pressed & INPUT_GAMEPAD_A) && have)
            download_and_launch(&m);
        if (reboot_countdown >= 0) {
            if (pressed & INPUT_GAMEPAD_B) {
                reboot_countdown = -1;
                failures = 0;
                auto_reboot = false;
                say("Reboot cancelled; auto-reboot is off until the launcher restarts.");
            } else if (reboot_countdown-- == 0) {
                set_reboots_in_a_row(reboots_in_a_row() + 1);
                say("Rebooting...");
                draw();
                reboot_console();
                say("The reboot call failed (see devlaunch.log). Reboot by hand.");
                auto_reboot = false;
            } else if (reboot_countdown % 60 == 0) {
                say("  rebooting in %d s...  (B: cancel)", reboot_countdown / 60 + 1);
            }
            continue;
        }
        if (countdown >= 0) {
            if (pressed & INPUT_GAMEPAD_B) {
                save_run(m.run);
                countdown = -1;
                say("Auto-run cancelled.");
            } else if (countdown-- == 0) {
                download_and_launch(&m);
            } else if (countdown % 60 == 0) {
                say("  %d...", countdown / 60 + 1);
            }
            continue;
        }
        /* Every 2 s (120 frames at 60 Hz): has the Mac asked for a new run? */
        if (server_ip[0] && frame % 120 == 0) {
            Manifest fresh;
            int fetched = fetch_manifest(&fresh);
            if (fetched != 0) {
                failures++;
                if (auto_reboot && failures >= FAILURES_BEFORE_REBOOT &&
                    reboots_in_a_row() < MAX_REBOOTS_IN_A_ROW) {
                    say(" ");
                    say("The Mac can't be reached (%d tries): the network is probably dead.", failures);
                    say("Rebooting the console in 10 s. B: cancel");
                    reboot_countdown = REBOOT_COUNTDOWN_FRAMES;
                }
            } else if (failures || !have) {
                failures = 0;
                set_reboots_in_a_row(0);
            }
            if (fetched == 0) {
                if (!have || strcmp(fresh.target, m.target) || fresh.run != m.run) {
                    m = fresh;
                    have = true;
                }
                if (fresh.run != last_run()) {
                    say(" ");
                    say("New run %ld from the Mac: %s starts in 3 s. B: cancel", fresh.run, fresh.target);
                    countdown = 180;
                }
            }
        }
    }
    xsdk_present_close();
    xsdk_text_close();
    xsdk_gpu_close();
    if (home_path[0]) {
        fflush(stdout);
        xsdk_launch(home_path, NULL, 0);
        printf("launch of %s failed (errno %d)\n", home_path, errno);
    }
    return 0;
}
