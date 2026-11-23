/* Where threads run: every pthread_create goes through placement.c, which puts the new thread on a hardware thread.
 * The console has 6 hardware threads, two per core: 0/1, 2/3, 4/5. */
#pragma once
#include <stdint.h>

/* Threads created from now on rotate through these hardware threads; count 0 restores the default (hw 1). */
void placement_set_workers(const uint32_t *hardware_threads, int count);
/* Priority for threads placed through placement_set_workers (default 0); the kernel keeps -3..4. */
void placement_set_worker_priority(int priority);
/* Priority for threads not placed as workers (Moonlight's network threads, the session thread...). Default +2. */
void placement_set_default_priority(int priority);
/* Moves the calling thread. */
void placement_move_self(uint32_t hardware_thread);
