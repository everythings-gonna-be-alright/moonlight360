#include "coverart.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "apps.h"
#include "image.h"

enum { SLOTS = 8 };

typedef struct {
    char host[64];
    int id;
    bool asked, done;   /* asked for; loaded (or found missing) */
    uint8_t *pixels;
    unsigned used;      /* when last asked for: the least recent is reused */
} Slot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static Slot slots[SLOTS];
static unsigned clock_now, generation;
static bool started;
static pthread_t thread;

static uint8_t *load(const char *host, int id)
{
    char path[160];
    snprintf(path, sizeof path, "covers/%s-%d.img", host, id);
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size <= 0)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *data = malloc((size_t)info.st_size);
    const size_t n = data ? fread(data, 1, (size_t)info.st_size, f) : 0;
    fclose(f);
    uint8_t *pixels = n ? image_decode_fit(data, n, APPS_COVER_W, APPS_COVER_H) : NULL;
    free(data);
    return pixels;
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        Slot *todo = NULL;
        for (int i = 0; i < SLOTS && !todo; i++)
            if (slots[i].asked && !slots[i].done)
                todo = &slots[i];
        if (!todo) {
            pthread_cond_wait(&wake, &lock);
            continue;
        }
        char host[64];
        snprintf(host, sizeof host, "%s", todo->host);
        const int id = todo->id;
        pthread_mutex_unlock(&lock);
        uint8_t *pixels = load(host, id);   /* the slow part, unlocked */
        pthread_mutex_lock(&lock);
        if (todo->id == id && !strcmp(todo->host, host) && !todo->done) {
            todo->pixels = pixels;
            todo->done = true;
            generation++;
        } else {
            free(pixels);   /* the slot went to another cover meanwhile */
        }
    }
    return NULL;
}

const uint8_t *coverart_get(const char *host, int id)
{
    if (id <= 0 || !host[0])
        return NULL;
    pthread_mutex_lock(&lock);
    clock_now++;
    Slot *slot = NULL, *oldest = &slots[0];
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].asked && slots[i].id == id && !strcmp(slots[i].host, host))
            slot = &slots[i];
        if (slots[i].used < oldest->used)
            oldest = &slots[i];
    }
    if (!slot) {   /* a new cover: take the least recently used slot */
        slot = oldest;
        free(slot->pixels);
        *slot = (Slot){ .id = id, .asked = true };
        snprintf(slot->host, sizeof slot->host, "%s", host);
        if (!started)
            started = pthread_create(&thread, NULL, worker, NULL) == 0;
        pthread_cond_signal(&wake);
    }
    slot->used = clock_now;
    const uint8_t *pixels = slot->done ? slot->pixels : NULL;
    pthread_mutex_unlock(&lock);
    return pixels;
}

unsigned coverart_generation(void)
{
    pthread_mutex_lock(&lock);
    const unsigned g = generation;
    pthread_mutex_unlock(&lock);
    return g;
}
