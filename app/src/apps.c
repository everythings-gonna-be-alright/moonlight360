#include "apps.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <client.h>
#include <errors.h>

#include "session.h"

typedef struct {
    char name[64];
    bool running;
} App;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static AppsState state;
static char message[256];
static App apps[APPS_MAX];
static int count;
static char host[64];
static pthread_t thread;
static bool thread_started;

static void finish(AppsState s, const char *text)
{
    pthread_mutex_lock(&lock);
    state = s;
    snprintf(message, sizeof message, "%s", text ? text : "");
    pthread_mutex_unlock(&lock);
    if (text)
        printf("[I] apps: %s\n", text);
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
        apps[count].running = server.currentGame != 0 && app->id == server.currentGame;
        count++;
    }
    pthread_mutex_unlock(&lock);
    while (list) {
        PAPP_LIST next = list->next;
        free(list->name);
        free(list);
        list = next;
    }
    gs_destroy(client);
    finish(count ? APPS_READY : APPS_FAILED, count ? NULL : "The host lists no apps.");
    return NULL;
}

void apps_fetch(const char *h)
{
    pthread_mutex_lock(&lock);
    bool busy = state == APPS_LOADING;
    pthread_mutex_unlock(&lock);
    if (busy)
        return;
    if (thread_started)
        pthread_join(thread, NULL);
    snprintf(host, sizeof host, "%s", h);
    pthread_mutex_lock(&lock);
    state = APPS_LOADING;
    message[0] = 0;
    count = 0;
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
