#include "apps.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include <client.h>
#include <errors.h>

#include "image.h"
#include "session.h"

#define COVERS_DIR "covers"

typedef struct {
    char name[64];
    int id;
    bool running;
    uint8_t *cover;   /* APPS_COVER_W x APPS_COVER_H RGBA once fetched; set under lock */
} App;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static AppsState state;
static char message[256];
static App apps[APPS_MAX];
static int count;
static char host[64];
static pthread_t thread;
static bool thread_started;
static unsigned generation;
static atomic_bool stop_covers;

static void finish(AppsState s, const char *text)
{
    pthread_mutex_lock(&lock);
    state = s;
    snprintf(message, sizeof message, "%s", text ? text : "");
    pthread_mutex_unlock(&lock);
    if (text)
        printf("[I] apps: %s\n", text);
}

/* Reads a whole file; NULL if it isn't there. */
static uint8_t *read_file(const char *path, size_t *size)
{
    struct stat info;
    if (stat(path, &info) != 0)   /* quietly: xsdk logs every open that fails, and a missing cover is normal */
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = n > 0 ? malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)n : 0;
    return data;
}

/* Each app's cover, in list order: kept files first, the host for the rest. A cover that won't come or decode stays
 * NULL, and the picker shows the app's initial. */
static void fetch_covers(GS_CLIENT client, const SERVER_DATA *server)
{
    mkdir(COVERS_DIR, 0777);
    int fetched = 0, kept = 0, failed = 0;
    for (int i = 0; i < count && !atomic_load(&stop_covers); i++) {
        char path[160];
        snprintf(path, sizeof path, COVERS_DIR "/%s-%d.img", host, apps[i].id);
        size_t size;
        uint8_t *file = read_file(path, &size);
        if (file)
            kept++;
        else if (gs_download_cover(client, server, apps[i].id, path) == GS_OK && (file = read_file(path, &size)))
            fetched++;
        uint8_t *cover = file ? image_decode_fit(file, size, APPS_COVER_W, APPS_COVER_H) : NULL;
        free(file);
        if (!cover) {
            failed++;
            remove(path);   /* not an image: fetch it again next time */
            continue;
        }
        pthread_mutex_lock(&lock);
        apps[i].cover = cover;
        pthread_mutex_unlock(&lock);
    }
    printf("[I] apps: covers: %d kept, %d fetched, %d missing%s\n", kept, fetched, failed,
           atomic_load(&stop_covers) ? " (stopped)" : "");
}

static void *fetch(void *arg)
{
    (void)arg;
    GS_CLIENT client = gs_new(SESSION_KEY_DIR);
    if (!client) {
        finish(APPS_FAILED, "Not paired yet: choose Connect once to pair, then pick an app.");
        return NULL;
    }
    static SERVER_DATA server;
    memset(&server, 0, sizeof server);
    int ret = gs_get_status(client, &server, host, 0, true);
    if (ret != GS_OK) {
        const char *e = NULL;
        gs_get_error(&e);
        char text[256];
        snprintf(text, sizeof text, "Could not reach %s: %s (%d)", host, e ? e : "error", ret);
        finish(APPS_FAILED, text);
        gs_destroy(client);
        return NULL;
    }
    if (!server.paired) {
        finish(APPS_FAILED, "Not paired with this PC yet: choose Connect once to pair, then pick an app.");
        gs_destroy(client);
        return NULL;
    }
    PAPP_LIST list = NULL;
    ret = gs_applist(client, &server, &list);
    if (ret != GS_OK) {
        char text[128];
        snprintf(text, sizeof text, "Could not get the app list (%d)", ret);
        finish(APPS_FAILED, text);
        gs_destroy(client);
        return NULL;
    }
    pthread_mutex_lock(&lock);
    count = 0;
    for (PAPP_LIST app = list; app && count < APPS_MAX; app = app->next) {
        snprintf(apps[count].name, sizeof apps[count].name, "%s", app->name);
        apps[count].id = app->id;
        apps[count].running = server.currentGame != 0 && app->id == server.currentGame;
        apps[count].cover = NULL;
        count++;
    }
    pthread_mutex_unlock(&lock);
    while (list) {
        PAPP_LIST next = list->next;
        free(list->name);
        free(list);
        list = next;
    }
    finish(count ? APPS_READY : APPS_FAILED, count ? NULL : "The host lists no apps.");
    fetch_covers(client, &server);
    gs_destroy(client);
    return NULL;
}

void apps_fetch(const char *h)
{
    pthread_mutex_lock(&lock);
    bool busy = state == APPS_LOADING;
    pthread_mutex_unlock(&lock);
    if (busy)
        return;
    if (thread_started) {
        atomic_store(&stop_covers, true);   /* the last fetch may still be on its covers */
        pthread_join(thread, NULL);
    }
    atomic_store(&stop_covers, false);
    snprintf(host, sizeof host, "%s", h);
    pthread_mutex_lock(&lock);
    state = APPS_LOADING;
    message[0] = 0;
    for (int i = 0; i < count; i++) {
        free(apps[i].cover);
        apps[i].cover = NULL;
    }
    count = 0;
    generation++;
    pthread_mutex_unlock(&lock);
    thread_started = pthread_create(&thread, NULL, fetch, NULL) == 0;
    if (!thread_started)
        finish(APPS_FAILED, "Could not start the app list thread.");
}

AppsState apps_state(void)
{
    pthread_mutex_lock(&lock);
    AppsState s = state;
    pthread_mutex_unlock(&lock);
    return s;
}

void apps_message(char *out, size_t size)
{
    pthread_mutex_lock(&lock);
    snprintf(out, size, "%s", message);
    pthread_mutex_unlock(&lock);
}

int apps_count(void)
{
    pthread_mutex_lock(&lock);
    int n = count;
    pthread_mutex_unlock(&lock);
    return n;
}

void apps_get(int i, char *name, size_t size, bool *running)
{
    pthread_mutex_lock(&lock);
    snprintf(name, size, "%s", i >= 0 && i < count ? apps[i].name : "");
    *running = i >= 0 && i < count && apps[i].running;
    pthread_mutex_unlock(&lock);
}

const uint8_t *apps_cover(int i)
{
    pthread_mutex_lock(&lock);
    const uint8_t *cover = i >= 0 && i < count ? apps[i].cover : NULL;
    pthread_mutex_unlock(&lock);
    return cover;
}

unsigned apps_generation(void)
{
    pthread_mutex_lock(&lock);
    const unsigned g = generation;
    pthread_mutex_unlock(&lock);
    return g;
}

void apps_stop_covers(void)
{
    atomic_store(&stop_covers, true);
}

int apps_id(int i)
{
    pthread_mutex_lock(&lock);
    const int id = i >= 0 && i < count ? apps[i].id : 0;
    pthread_mutex_unlock(&lock);
    return id;
}
