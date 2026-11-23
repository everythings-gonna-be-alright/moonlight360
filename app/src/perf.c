#include "perf.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/thread.h"

enum {
    HW_THREADS = 6,
    PROBE_SLEEP_MS = 1,
    SAMPLE_MS = 500,
    GPU_SLOTS = 8,               /* frames of clock stamps in flight */
    GPU_CLOCKS_PER_MS = 498750,  /* xsdk gpu.h: 498.75 a microsecond */
    /* HalSendSMCMessage: xboxkrnl ordinal 0x29, beside HalReturnToFirmware (0x28) that devlaunch reboots with. */
    SMC_ORDINAL = 0x29,
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static PerfReadings readings = { .cpu = { -1, -1, -1, -1, -1, -1 }, .cpu_average = -1, .gpu = -1 };
static bool started;

static atomic_uint rounds[HW_THREADS];
static atomic_uint best_round_us[HW_THREADS];   /* the shortest round seen: what an idle hardware thread gives */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

/* A probe per hardware thread at -16, the lowest priority (xsdk docs/GUIDE.md, "Priority requests"): it sleeps a
 * millisecond at a time, so it runs only in the gaps the other threads there leave. */
static uint32_t probe(void *context)
{
    const uint32_t hw = (uint32_t)(uintptr_t)context;
    xsdk_thread_set_priority(XSDK_CURRENT_THREAD, XSDK_PRIORITY_MIN, NULL);
    atomic_store(&best_round_us[hw], 0xFFFFFFFFu);
    uint64_t last = now_us();
    for (;;) {
        xsdk_thread_sleep(PROBE_SLEEP_MS);
        const uint64_t now = now_us();
        const uint32_t round = (uint32_t)(now - last);
        last = now;
        if (round < atomic_load(&best_round_us[hw]))
            atomic_store(&best_round_us[hw], round);
        atomic_fetch_add(&rounds[hw], 1);
    }
    return 0;
}

/* ---- temperatures ---- */

typedef void (*SmcMessage)(const uint8_t *request, uint8_t *reply);
static SmcMessage smc;

static void find_smc(void)
{
    HANDLE kernel = 0;
    void *address = NULL;
    uint32_t found = XexGetModuleHandle("xboxkrnl.exe", &kernel);
    NTSTATUS status = found == 0 ? XexGetProcedureAddress(kernel, SMC_ORDINAL, &address) : (NTSTATUS)found;
    printf("[I] perf: HalSendSMCMessage: module 0x%08X, export 0x%08X, %p\n", (unsigned)found, (unsigned)status,
           address);
    if (found == 0 && NT_SUCCESS(status))
        smc = (SmcMessage)address;
}

/* The system controller's sensor query, 0x07: four 8.8 fixed-point readings, little-endian, CPU, GPU, eDRAM and
 * board (libxenon drivers/xenon_smc/xenon_smc.c, xenon_smc_query_sensors). */
static void read_temperatures(PerfReadings *r)
{
    if (!smc)
        return;
    uint8_t request[16] = { 0x07 }, reply[16];
    memset(reply, 0, sizeof reply);
    smc(request, reply);
    float t[4];
    for (int i = 0; i < 4; i++)
        t[i] = (float)(reply[i * 2 + 1] | reply[i * 2 + 2] << 8) / 256.0f;
    /* Only plausible readings: a reply in another layout shows nothing rather than nonsense. */
    for (int i = 0; i < 4; i++)
        if (t[i] < 10.0f || t[i] > 125.0f)
            return;
    r->temp_cpu = t[0];
    r->temp_gpu = t[1];
    r->temp_edram = t[2];
    r->temp_board = t[3];
}

/* ---- the sampler ---- */

static atomic_uint_fast64_t gpu_busy_ticks;

/* Twice a second: a probe's rounds times its idle round time is its hardware thread's idle share; the rest is load
 * (the app's, XAM's and the kernel's alike). */
static uint32_t sampler(void *context)
{
    (void)context;
    unsigned last_rounds[HW_THREADS] = { 0 };
    uint64_t last = now_us();
    uint64_t last_gpu = atomic_load(&gpu_busy_ticks);
    bool logged = false;
    for (;;) {
        xsdk_thread_sleep(SAMPLE_MS);
        const uint64_t now = now_us();
        const double elapsed_us = (double)(now - last);
        last = now;
        PerfReadings r;
        pthread_mutex_lock(&lock);
        r = readings;
        pthread_mutex_unlock(&lock);
        int sum = 0, counted = 0;
        for (int hw = 0; hw < HW_THREADS; hw++) {
            const unsigned n = atomic_load(&rounds[hw]);
            const unsigned done = n - last_rounds[hw];
            last_rounds[hw] = n;
            const uint32_t best = atomic_load(&best_round_us[hw]);
            if (best == 0xFFFFFFFFu || best == 0) {
                r.cpu[hw] = -1;
                continue;
            }
            const double idle = (double)done * best / elapsed_us;
            const int load = (int)(100.0 * (1.0 - (idle > 1.0 ? 1.0 : idle)) + 0.5);
            r.cpu[hw] = load;
            sum += load;
            counted++;
        }
        r.cpu_average = counted ? sum / counted : -1;
        const uint64_t g = atomic_load(&gpu_busy_ticks);
        const double gpu_ms = (double)(g - last_gpu) / GPU_CLOCKS_PER_MS;
        last_gpu = g;
        const int gpu = (int)(100.0 * gpu_ms / (elapsed_us / 1000.0) + 0.5);
        r.gpu = gpu > 100 ? 100 : gpu;
        if (r.sample % 2 == 0)   /* temperatures change slowly: the system controller is asked once a second */
            read_temperatures(&r);
        r.sample++;
        pthread_mutex_lock(&lock);
        readings = r;
        pthread_mutex_unlock(&lock);
        if (!logged && r.temp_cpu > 0) {
            printf("[I] perf: temperatures CPU %.1f, GPU %.1f, eDRAM %.1f, board %.1f C\n", r.temp_cpu, r.temp_gpu,
                   r.temp_edram, r.temp_board);
            logged = true;
        }
    }
    return 0;
}

void perf_start(void)
{
    if (started)
        return;
    started = true;
    find_smc();
    for (uint32_t hw = 0; hw < HW_THREADS; hw++) {
        HANDLE h;
        if (!NT_SUCCESS(xsdk_thread_create_on(&h, 16 * 1024, probe, (void *)(uintptr_t)hw, hw)))
            printf("[W] perf: probe on hw %u failed\n", (unsigned)hw);
    }
    HANDLE h;
    if (!NT_SUCCESS(xsdk_thread_create_on(&h, 16 * 1024, sampler, NULL, 1)))
        printf("[W] perf: sampler failed\n");
}

void perf_get(PerfReadings *out)
{
    pthread_mutex_lock(&lock);
    *out = readings;
    pthread_mutex_unlock(&lock);
}

/* ---- the GPU's share: a clock stamp as each frame's commands start, and one once the GPU is idle after it ---- */

static volatile uint32_t *stamps;   /* GPU_SLOTS x (start, end) x XSDK_GPU_CLOCK_WORDS */
static uint32_t gpu_frame;

static volatile uint32_t *stamp(uint32_t frame, int end)
{
    return stamps + ((frame % GPU_SLOTS) * 2 + (uint32_t)end) * XSDK_GPU_CLOCK_WORDS;
}

void perf_gpu_frame_begin(void)
{
    if (!stamps) {
        stamps = xsdk_gpu_alloc(GPU_SLOTS * 2 * XSDK_GPU_CLOCK_WORDS * 4);
        if (!stamps)
            return;
        for (uint32_t i = 0; i < GPU_SLOTS * 2 * XSDK_GPU_CLOCK_WORDS; i++)
            stamps[i] = 0;
    }
    /* The frame GPU_SLOTS - 2 back is long done (two frames in flight at most): add its time, free its slot. */
    if (gpu_frame >= GPU_SLOTS - 2) {
        const uint32_t old = gpu_frame - (GPU_SLOTS - 2);
        const uint64_t a = xsdk_gpu_clock_value(stamp(old, 0)), b = xsdk_gpu_clock_value(stamp(old, 1));
        if (a && b > a && b - a < 500ull * GPU_CLOCKS_PER_MS)
            atomic_fetch_add(&gpu_busy_ticks, b - a);
    }
    xsdk_gpu_write_clock(xsdk_gpu_address(stamp(gpu_frame, 0)), false);
}

void perf_gpu_frame_end(void)
{
    if (!stamps)
        return;
    xsdk_gpu_write_clock(xsdk_gpu_address(stamp(gpu_frame, 1)), true);
    gpu_frame++;
}
