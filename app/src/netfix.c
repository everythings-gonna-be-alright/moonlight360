#include "netfix.h"

#include <stdint.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "xsdk/kernel.h"
#include "xsdk/net.h"

/* XNetStartupParams, as xsdk's src/socket.c builds it: 13 bytes, its size first. */
enum {
    STARTUP_PARAMS_BYTES = 13,
    STARTUP_DATAGRAM_SOCKETS = 2,
    STARTUP_STREAM_SOCKETS = 3,
    STARTUP_RECEIVE_KB = 4,
    STARTUP_SEND_KB = 5,
    WINSOCK_VERSION = 0x0202,
    WSADATA_BYTES = 400,
    /* The largest receive buffer xsdk has seen XAM honour on the console: asking for 256 KB read back 261120
     * (xsdk docs/HARDWARE.md, milestone 56). */
    MAX_RCVBUF_BYTES = 261120,
};

void netfix_restart_network(void)
{
    const XsdkNetworkLimits limits = xsdk_network_limits;
    const uint8_t params[STARTUP_PARAMS_BYTES] = {
        [0] = STARTUP_PARAMS_BYTES,
        [STARTUP_DATAGRAM_SOCKETS] = limits.datagram_sockets,
        [STARTUP_STREAM_SOCKETS] = limits.stream_sockets,
        [STARTUP_RECEIVE_KB] = limits.receive_kb,
        [STARTUP_SEND_KB] = limits.send_kb,
    };
    static uint8_t wsadata[WSADATA_BYTES];
    int32_t wsa_cleanup = NetDll_WSACleanup(XNCALLER_TITLE);
    int32_t xnet_cleanup = NetDll_XNetCleanup(XNCALLER_TITLE, NULL);
    int32_t xnet_startup = NetDll_XNetStartup(XNCALLER_TITLE, params);
    int32_t wsa_startup = NetDll_WSAStartup(XNCALLER_TITLE, WINSOCK_VERSION, wsadata);
    printf("[I] netfix: network restarted (WSACleanup %d, XNetCleanup %d, XNetStartup %d, WSAStartup %d)\n",
           (int)wsa_cleanup, (int)xnet_cleanup, (int)xnet_startup, (int)wsa_startup);
}

/* Every setsockopt goes through here (-Wl,--wrap=setsockopt). moonlight-common-c asks for a ~2.9 MB SO_RCVBUF on
 * the video socket and keeps the first size accepted. XAM accepts sizes it doesn't honour, so the request is capped
 * at the largest size known to work rather than leaving XAM to make sense of it. */
int __real_setsockopt(int fd, int level, int name, const void *value, socklen_t length);

/* Non-blocking waits (netfix_set_nonblocking_waits): per socket, the receive timeout the caller asked for with
 * SO_RCVTIMEO, emulated here instead of handed to the stack. 0: not emulated. */
enum { NB_FIRST_FD = 35, NB_SLOTS = 232 };
static bool nonblocking_waits;
static int emulated_timeout_ms[NB_SLOTS];

void netfix_set_nonblocking_waits(bool on)
{
    nonblocking_waits = on;
    printf("[I] netfix: waits %s\n", on ? "NON-BLOCKING (1 ms sleeps in the title, never blocked in the stack)"
                                        : "blocking in the stack (default)");
}

static uint64_t nb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void nb_sleep_1ms(void)
{
    struct timespec ts = { 0, 1000000 };
    nanosleep(&ts, NULL);
}

static int logged_options;

static int log_setsockopt(int fd, int level, int name, const void *value, socklen_t length, int result)
{
    /* Every call, as the console's stack (Winsock numbering) may read a Linux-numbered option differently. */
    if (logged_options++ < 200) {
        long shown = value && length >= sizeof(int) ? (long)*(const int *)value : -1;
        printf("[I] netfix: setsockopt(fd %d, level 0x%x, option 0x%x, value %ld, %u bytes) = %d\n", fd, level, name,
               shown, (unsigned)length, result);
    }
    return result;
}

int __wrap_setsockopt(int fd, int level, int name, const void *value, socklen_t length)
{
    int nb_slot = fd - NB_FIRST_FD;
    if (nonblocking_waits && level == SOL_SOCKET && name == SO_RCVTIMEO && value && length == sizeof(struct timeval) &&
        nb_slot >= 0 && nb_slot < NB_SLOTS) {
        const struct timeval *tv = value;
        int ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
        emulated_timeout_ms[nb_slot] = ms > 0 ? ms : 1;
        int flags = fcntl(fd, F_GETFL);
        int result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        printf("[I] netfix: fd %d receive timeout %d ms emulated, socket non-blocking (%d)\n", fd, ms, result);
        return result;
    }
    if (level == SOL_SOCKET && name == SO_RCVBUF && value && length == sizeof(int)) {
        int requested = *(const int *)value;
        if (requested > MAX_RCVBUF_BYTES) {
            static int logged;
            if (!logged++)
                printf("[I] netfix: SO_RCVBUF %d capped to %d\n", requested, MAX_RCVBUF_BYTES);
            int capped = MAX_RCVBUF_BYTES;
            return log_setsockopt(fd, level, name, &capped, sizeof capped,
                                  __real_setsockopt(fd, level, name, &capped, sizeof capped));
        }
    }
    return log_setsockopt(fd, level, name, value, length, __real_setsockopt(fd, level, name, value, length));
}

/* Every connect, to see which UDP sockets are connected and to where (-Wl,--wrap=connect). */
int __real_connect(int fd, const struct sockaddr *address, socklen_t length);

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    int result = __real_connect(fd, address, length);
    int type = 0;
    socklen_t type_length = sizeof type;
    getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length);
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;
    if (address && address->sa_family == AF_INET) {
        const unsigned char *ip = (const unsigned char *)&in->sin_addr;
        printf("[I] netfix: connect(fd %d, %s, %u.%u.%u.%u:%u) = %d\n", fd, type == SOCK_DGRAM ? "UDP" : "TCP", ip[0],
               ip[1], ip[2], ip[3], (unsigned)ntohs(in->sin_port), result);
    }
    return result;
}

/* Every recvfrom goes through here (-Wl,--wrap=recvfrom): the largest datagram seen on each socket and how many
 * were over 1400 bytes, to see what the host really sends. */
enum { FIRST_SOCKET_FD = 35, SOCKET_SLOTS = 232, BIG_BYTES = 1400 };
static atomic_uint largest[SOCKET_SLOTS], big_count[SOCKET_SLOTS], total_count[SOCKET_SLOTS];

ssize_t __real_recvfrom(int fd, void *buffer, size_t length, int flags, struct sockaddr *from, socklen_t *from_length);

ssize_t __wrap_recvfrom(int fd, void *buffer, size_t length, int flags, struct sockaddr *from, socklen_t *from_length)
{
    ssize_t n = __real_recvfrom(fd, buffer, length, flags, from, from_length);
    int nb = fd - NB_FIRST_FD;
    if (n < 0 && nb >= 0 && nb < NB_SLOTS && emulated_timeout_ms[nb] && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        /* The caller wanted a receive that waits up to its timeout: wait here, asleep, and look again. */
        uint64_t deadline = nb_now_ms() + (uint64_t)emulated_timeout_ms[nb];
        do {
            nb_sleep_1ms();
            n = __real_recvfrom(fd, buffer, length, flags, from, from_length);
        } while (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && nb_now_ms() < deadline);
        if (n < 0 && errno == EWOULDBLOCK)
            errno = EAGAIN;   /* as a timed-out blocking receive reports */
    }
    int slot = fd - FIRST_SOCKET_FD;
    if (n > 0 && slot >= 0 && slot < SOCKET_SLOTS) {
        atomic_fetch_add(&total_count[slot], 1);
        if ((unsigned)n > BIG_BYTES)
            atomic_fetch_add(&big_count[slot], 1);
        unsigned seen = atomic_load(&largest[slot]);
        while ((unsigned)n > seen && !atomic_compare_exchange_weak(&largest[slot], &seen, (unsigned)n)) {
        }
    }
    return n;
}

void netfix_log_datagrams(void)
{
    for (int i = 0; i < SOCKET_SLOTS; i++) {
        unsigned total = atomic_load(&total_count[i]);
        if (total)
            printf("[I] netfix: socket %d: %u datagrams, largest %u bytes, %u over %d bytes\n", i + FIRST_SOCKET_FD,
                   total, atomic_load(&largest[i]), atomic_load(&big_count[i]), BIG_BYTES);
    }
}

void netfix_reset_datagrams(void)
{
    for (int i = 0; i < SOCKET_SLOTS; i++) {
        atomic_store(&largest[i], 0);
        atomic_store(&big_count[i], 0);
        atomic_store(&total_count[i], 0);
    }
}

static pthread_t keepalive_thread;
static atomic_int keepalive_pps;
static bool keepalive_running;
static struct sockaddr_in keepalive_to;

static void *keepalive_loop(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return NULL;
    static const char payload[32] = "moonlight360 keepalive";
    unsigned sent = 0;
    int pps;
    while ((pps = atomic_load(&keepalive_pps)) > 0) {
        if (sendto(fd, payload, sizeof payload, 0, (struct sockaddr *)&keepalive_to, sizeof keepalive_to) > 0)
            sent++;
        struct timespec ts = { 0, 1000000000L / pps };
        nanosleep(&ts, NULL);
    }
    close(fd);
    printf("[I] netfix: keepalive stopped after %u packets\n", sent);
    return NULL;
}

void netfix_keepalive(const char *host, int pps)
{
    if (keepalive_running) {
        atomic_store(&keepalive_pps, 0);
        pthread_join(keepalive_thread, NULL);
        keepalive_running = false;
    }
    if (pps <= 0)
        return;
    keepalive_to = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(9) };
    inet_pton(AF_INET, host, &keepalive_to.sin_addr);
    atomic_store(&keepalive_pps, pps);
    keepalive_running = pthread_create(&keepalive_thread, NULL, keepalive_loop, NULL) == 0;
    printf("[I] netfix: keepalive %d packets/s to %s:9\n", pps, host);
}

/* Every poll (-Wl,--wrap=poll). With non-blocking waits on, the stack is only ever asked "ready now?" (timeout 0). */
int __real_poll(struct pollfd *fds, nfds_t count, int timeout_ms);

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout_ms)
{
    if (!nonblocking_waits || timeout_ms == 0)
        return __real_poll(fds, count, timeout_ms);
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : nb_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        int ready = __real_poll(fds, count, 0);
        if (ready != 0 || nb_now_ms() >= deadline)
            return ready;
        nb_sleep_1ms();
    }
}

void netfix_snapshot(char *out, size_t size)
{
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < SOCKET_SLOTS && at + 24 < size; i++) {
        unsigned total = atomic_load(&total_count[i]);
        if (total)
            at += (size_t)snprintf(out + at, size - at, "%d:%u ", i + FIRST_SOCKET_FD, total);
    }
}
