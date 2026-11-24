#include "discover.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "xsdk/net.h"

enum {
    HTTP_PORT = 47989,       /* GameStream's plain HTTP port: /serverinfo answers without pairing */
    WORKERS = 12,            /* addresses tried at once: XAM allows 32 TCP sockets in all */
    CONNECT_MS = 600,        /* a PC on the LAN answers well within this; no one there never does */
    REPLY_MS = 2000,
    ADDRESSES = 254,
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char address[16];
    char name[64];
} found[DISCOVER_MAX];
static int count;
static atomic_int next_host, tried, workers_left;
static atomic_bool stop;
static uint32_t base;   /* the /24's first address, host order */
static bool started;
static pthread_t threads[WORKERS];

/* connect without its wrapper's log line (netfix.c): 254 of them would fill the log. */
int __real_connect(int fd, const struct sockaddr *address, socklen_t length);

/* The text between <tag> and </tag> in xml, into out; false if there's none. */
static bool xml_value(const char *xml, const char *tag, char *out, size_t size)
{
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = strstr(xml, open);
    const char *b = a ? strstr(a, close) : NULL;
    if (!a || !b)
        return false;
    a += strlen(open);
    const size_t n = (size_t)(b - a) < size - 1 ? (size_t)(b - a) : size - 1;
    memcpy(out, a, n);
    out[n] = 0;
    return n > 0;
}

/* Whether a GameStream host answers at ip; its name into name. */
static bool probe(uint32_t ip, char *name, size_t name_size)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    bool ok = false;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(HTTP_PORT) };
    to.sin_addr.s_addr = htonl(ip);
    int r = __real_connect(fd, (const struct sockaddr *)&to, sizeof to);
    if (r != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK)
        goto out;
    if (r != 0) {
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        if (poll(&p, 1, CONNECT_MS) != 1 || (p.revents & (POLLERR | POLLHUP)))
            goto out;
    }
    /* Connected (XAM gives no SO_ERROR: the reply is what tells). Blocking again, with a timeout. */
    fcntl(fd, F_SETFL, 0);
    const struct timeval timeout = { .tv_sec = REPLY_MS / 1000, .tv_usec = (REPLY_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    char request[160];
    const unsigned char *b = (const unsigned char *)&to.sin_addr;
    const int n = snprintf(request, sizeof request,
                           "GET /serverinfo?uniqueid=0123456789ABCDEF HTTP/1.0\r\nHost: %u.%u.%u.%u:%d\r\n\r\n", b[0],
                           b[1], b[2], b[3], HTTP_PORT);
    if (send(fd, request, (size_t)n, 0) != n)
        goto out;
    char reply[3072];   /* the host's name comes early: a longer reply is cut, which is fine */
    size_t got = 0;
    for (;;) {
        const ssize_t k = recv(fd, reply + got, sizeof reply - 1 - got, 0);
        if (k <= 0)
            break;
        got += (size_t)k;
        if (got == sizeof reply - 1)
            break;
    }
    reply[got] = 0;
    if (!strstr(reply, "<root"))
        goto out;
    if (!xml_value(reply, "hostname", name, name_size))
        name[0] = 0;
    ok = true;
out:
    close(fd);
    return ok;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        const int host = atomic_fetch_add(&next_host, 1);
        if (host > ADDRESSES || atomic_load(&stop))
            break;
        const uint32_t ip = base + (uint32_t)host;
        char name[64];
        if (probe(ip, name, sizeof name)) {
            char address[16];
            snprintf(address, sizeof address, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
            printf("[I] discover: %s at %s\n", name[0] ? name : "a host", address);
            pthread_mutex_lock(&lock);
            if (count < DISCOVER_MAX) {
                snprintf(found[count].address, sizeof found[count].address, "%s", address);
                snprintf(found[count].name, sizeof found[count].name, "%s", name[0] ? name : address);
                count++;
            }
            pthread_mutex_unlock(&lock);
        }
        atomic_fetch_add(&tried, 1);
    }
    atomic_fetch_sub(&workers_left, 1);
    return NULL;
}

void discover_start(void)
{
    if (discover_running())
        return;
    if (started)
        for (int i = 0; i < WORKERS; i++)
            pthread_join(threads[i], NULL);
    started = false;
    pthread_mutex_lock(&lock);
    count = 0;
    pthread_mutex_unlock(&lock);
    atomic_store(&tried, 0);
    atomic_store(&stop, false);
    struct in_addr own;
    if (xsdk_net_address(&own) != 0) {
        printf("[W] discover: the console has no address yet\n");
        atomic_store(&tried, ADDRESSES);
        return;
    }
    base = ntohl(own.s_addr) & 0xFFFFFF00u;
    atomic_store(&next_host, 1);
    atomic_store(&workers_left, WORKERS);
    printf("[I] discover: searching %u.%u.%u.0/24\n", base >> 24, (base >> 16) & 0xFF, (base >> 8) & 0xFF);
    for (int i = 0; i < WORKERS; i++)
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
            atomic_fetch_sub(&workers_left, 1);
            threads[i] = 0;
        }
    started = true;
}

bool discover_running(void)
{
    return started && atomic_load(&workers_left) > 0;
}

void discover_progress(int *t, int *total)
{
    *t = atomic_load(&tried);
    *total = ADDRESSES;
}

int discover_count(void)
{
    pthread_mutex_lock(&lock);
    const int n = count;
    pthread_mutex_unlock(&lock);
    return n;
}

void discover_get(int i, char *address, size_t address_size, char *name, size_t name_size)
{
    pthread_mutex_lock(&lock);
    snprintf(address, address_size, "%s", i >= 0 && i < count ? found[i].address : "");
    snprintf(name, name_size, "%s", i >= 0 && i < count ? found[i].name : "");
    pthread_mutex_unlock(&lock);
}

void discover_stop(void)
{
    atomic_store(&stop, true);
}
