#include "gpudec.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>

#include "gpumc.h"
#include "xsdk/thread.h"

/* h264_xenon.h: FFmpeg's internal header, its prototypes (patches/ffmpeg-xenon-vmx128.patch). */
void xenon_hybrid_set_grid(uint32_t *mv_grid, int pitch_blocks, int rows_blocks);
void xenon_hybrid_set_residual(int16_t *luma, int luma_pitch, int16_t *cb, int16_t *cr, int chroma_pitch,
                               uint8_t *flags, int flags_pitch_bytes);
void xenon_hybrid_set_residual_layout(const uint32_t *const offsets[3], const uint32_t texels[3],
                                      const uint32_t band[3]);
void xenon_hybrid_enable(int on);
void xenon_hybrid_begin_frame(void);
int xenon_hybrid_gpu_needed(void);
int xenon_hybrid_failed(void);
int xenon_hybrid_reference(uint8_t *planes[3]);
int xenon_hybrid_contexts(void);
int xenon_hybrid_replay(int index);

enum {
    REPLAY_THREADS = 6,
    JOB_TIMEOUT_MS = 1000,
};

static bool wanted, active;

void gpudec_set_wanted(bool on)
{
    wanted = on;
}

bool gpudec_wanted(void)
{
    return wanted;
}

bool gpudec_active(void)
{
    return active;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

/* ---- jobs for the drawing thread, which alone may use the GPU (xsdk docs/GUIDE.md, "Threads") ---- */

typedef enum { JOB_NONE, JOB_INIT, JOB_PREDICT } JobKind;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static struct {
    JobKind kind;       /* asked for, not yet taken */
    bool done, ok;
    int width, height;
    uint8_t *ref[3], *dst[3];
    int pitch[3];
    uint32_t gpu_us;
} job;

static bool run_job(JobKind kind)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += JOB_TIMEOUT_MS / 1000;
    pthread_mutex_lock(&job_lock);
    job.kind = kind;
    job.done = false;
    while (!job.done)
        if (pthread_cond_timedwait(&job_cond, &job_lock, &deadline) != 0)
            break;
    const bool ok = job.done && job.ok;
    if (!job.done)
        job.kind = JOB_NONE;   /* never taken: the drawing thread isn't looping */
    pthread_mutex_unlock(&job_lock);
    return ok;
}

void gpudec_service(void)
{
    pthread_mutex_lock(&job_lock);
    const JobKind kind = job.kind;
    job.kind = JOB_NONE;
    pthread_mutex_unlock(&job_lock);
    if (kind == JOB_NONE)
        return;
    bool ok = false;
    const uint64_t t0 = now_us();
    if (kind == JOB_INIT) {
        static bool calibrated;
        static int ready_w, ready_h;
        if (ready_w && (ready_w != job.width || ready_h != job.height)) {
            gpumc_shutdown();
            ready_w = ready_h = 0;
            calibrated = false;
        }
        ok = gpumc_init(job.width, job.height);
        if (ok && !calibrated) {
            char report[200];
            ok = calibrated = gpumc_calibrate(report, sizeof report);
            printf("[I] gpudec: calibration: %s%s\n", report, ok ? "" : " -- GPU decoding off");
        }
        if (ok) {
            ready_w = job.width;
            ready_h = job.height;
        }
    } else {
        GpuMcTimes t = { .split = false, .sdk_download = false };
        ok = gpumc_predict(job.ref, job.dst, job.pitch, &t);
    }
    pthread_mutex_lock(&job_lock);
    job.ok = ok;
    job.done = true;
    job.gpu_us = (uint32_t)(now_us() - t0);
    pthread_cond_broadcast(&job_cond);
    pthread_mutex_unlock(&job_lock);
}

/* ---- replay threads: FFmpeg's rebuild of the kept macroblocks, a slice context each, on every hardware thread ---- */

static sem_t replay_start[REPLAY_THREADS], replay_done;
static atomic_int replay_next, replay_count, replay_errors;
static bool replay_threads;

static uint32_t replay_worker(void *arg)
{
    const int index = (int)(intptr_t)arg;
    for (;;) {
        sem_wait(&replay_start[index]);
        for (int i; (i = atomic_fetch_add(&replay_next, 1)) < atomic_load(&replay_count);)
            if (xenon_hybrid_replay(i) < 0)
                atomic_fetch_add(&replay_errors, 1);
        sem_post(&replay_done);
    }
    return 0;
}

static bool start_replay_threads(void)
{
    if (replay_threads)
        return true;
    sem_init(&replay_done, 0, 0);
    for (int i = 0; i < REPLAY_THREADS; i++) {
        sem_init(&replay_start[i], 0, 0);
        HANDLE h;
        /* Below the threads that must not wait (main +1, network +2), as the decode workers are (video.c). */
        if (!NT_SUCCESS(xsdk_thread_create_on(&h, 64 * 1024, replay_worker, (void *)(intptr_t)i, (uint32_t)i)))
            return false;
    }
    replay_threads = true;
    return true;
}

static bool replay_all(void)
{
    atomic_store(&replay_next, 0);
    atomic_store(&replay_errors, 0);
    atomic_store(&replay_count, xenon_hybrid_contexts());
    for (int i = 0; i < REPLAY_THREADS; i++)
        sem_post(&replay_start[i]);
    for (int i = 0; i < REPLAY_THREADS; i++)
        sem_wait(&replay_done);
    return atomic_load(&replay_errors) == 0;
}

/* ---- the decode thread's side ---- */

static uint64_t window_start, window_gpu, window_replay;
static uint32_t window_frames;
static float gpu_ms_avg, replay_ms_avg;

bool gpudec_start(int coded_width, int coded_height)
{
    active = false;
    if (!wanted)
        return false;
    job.width = coded_width;
    job.height = coded_height;
    if (!start_replay_threads() || !run_job(JOB_INIT)) {
        printf("[W] gpudec: GPU decoding couldn't start; the CPU decodes\n");
        return false;
    }
    int grid_pitch, grid_rows, res_pitch[2], flags_pitch;
    int16_t *res[3];
    uint8_t *flags;
    uint32_t *grid = gpumc_grid(&grid_pitch, &grid_rows);
    gpumc_residual(res, res_pitch, &flags, &flags_pitch);
    xenon_hybrid_set_grid(grid, grid_pitch, grid_rows);
    xenon_hybrid_set_residual(res[0], res_pitch[0], res[1], res[2], res_pitch[1], flags, flags_pitch);
    const uint32_t *offsets[3];
    uint32_t texels[3], band[3];
    gpumc_residual_layout(offsets, texels, band);
    xenon_hybrid_set_residual_layout(offsets, texels, band);
    xenon_hybrid_enable(1);
    active = true;
    window_start = now_us();
    window_gpu = window_replay = 0;
    window_frames = 0;
    printf("[I] gpudec: GPU decoding on (experimental), %dx%d coded\n", coded_width, coded_height);
    return true;
}

void gpudec_begin_frame(void)
{
    if (active)
        xenon_hybrid_begin_frame();
}

static void turn_off(const char *why)
{
    printf("[W] gpudec: %s; GPU decoding off for this stream, the CPU decodes\n", why);
    xenon_hybrid_enable(0);
    active = false;
}

bool gpudec_finish_frame(AVFrame *f)
{
    if (!active)
        return true;
    if (xenon_hybrid_failed()) {
        static bool logged;
        if (!logged)
            printf("[W] gpudec: FFmpeg's hook reported a failure (%d)\n", xenon_hybrid_failed());
        logged = true;
    }
    const uint64_t t0 = now_us();
    uint32_t gpu_us = 0;
    uint8_t *ref[3];
    if (xenon_hybrid_gpu_needed()) {
        if (!xenon_hybrid_reference(ref)) {
            turn_off("no reference picture for the GPU");
            return false;
        }
        pthread_mutex_lock(&job_lock);
        for (int p = 0; p < 3; p++) {
            job.ref[p] = ref[p];
            job.dst[p] = f->data[p];
            job.pitch[p] = f->linesize[p];
        }
        pthread_mutex_unlock(&job_lock);
        if (!run_job(JOB_PREDICT)) {
            turn_off("the GPU's prediction failed or never ran");
            return false;
        }
        gpu_us = job.gpu_us;
    }
    const uint64_t t1 = now_us();
    if (!replay_all()) {
        turn_off("the replay found FFmpeg on another picture");
        return false;
    }
    const uint64_t t2 = now_us();
    window_gpu += gpu_us ? (uint64_t)(t1 - t0) : 0;
    window_replay += t2 - t1;
    window_frames++;
    if (t2 - window_start >= 1000000) {
        gpu_ms_avg = window_frames ? window_gpu / 1000.0f / window_frames : 0;
        replay_ms_avg = window_frames ? window_replay / 1000.0f / window_frames : 0;
        window_start = t2;
        window_gpu = window_replay = 0;
        window_frames = 0;
    }
    return true;
}

void gpudec_stop(void)
{
    if (active)
        xenon_hybrid_enable(0);
    active = false;
}

void gpudec_times(float *gpu_ms, float *replay_ms)
{
    *gpu_ms = gpu_ms_avg;
    *replay_ms = replay_ms_avg;
}
