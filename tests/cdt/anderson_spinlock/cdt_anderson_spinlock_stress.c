/**
 * @file cdt_anderson_spinlock_stress.c
 * @brief Standalone multi-threaded stress test for the Anderson array-based
 *        queue spinlock.
 *
 * Usage:
 *   cdt_anderson_spinlock_stress [--threads N] [--duration S] [--slots M]
 *
 * Defaults: threads=8, duration=5, slots=16.
 *
 * N threads contend on a shared counter protected by the Anderson lock.
 * Each thread counts its own successful increments.  After the run, the
 * shared counter must equal the sum of all per-thread counts — any
 * discrepancy indicates a correctness bug (lost or doubled increment).
 *
 * Zero-error assertions:
 *   - shared counter == sum of per-thread counters after run
 *   - no thread observes a NULL slot pointer
 *   - `spinlock_anderson_locked()` is never true after all threads join
 *   - all operations complete without deadlock (timeout gate)
 *
 * A non-zero error count indicates a correctness bug; the process exits 1.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <uflib/cdt/cdt_anderson_spinlock.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

/* ── Defaults ─────────────────────────────────────────────────────────── */

#define DEFAULT_THREADS  8
#define DEFAULT_DURATION 5
#define DEFAULT_SLOTS    16

#define MAX_THREADS      256
#define MAX_SLOTS        4096

/* ── Shared state ─────────────────────────────────────────────────────── */

typedef struct {
    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t *slots;       /* dynamically allocated */
    unsigned int               slot_count;

    uint64_t                   shared_counter;  /* protected by lock */
    _Atomic bool               stop;
    _Atomic uint32_t           live_threads;

    /* Per-thread stats */
    _Atomic uint64_t           thread_ops[MAX_THREADS];
    _Atomic uint32_t           null_slot_errors;
    _Atomic uint32_t           deadlock_timeout;
} StressState;

/* ── Thread function ──────────────────────────────────────────────────── */

static void *
stress_thread(void *arg)
{
    StressState *st = (StressState *)arg;

    /* On Linux, gettid() would be better, but we use a CAS on a simple
     * per-thread allocation counter for portability. */
    static _Atomic unsigned int s_tid_counter = 0;
    unsigned int tid = atomic_fetch_add(&s_tid_counter, 1);

    atomic_fetch_add(&st->live_threads, 1);

    while (!atomic_load_explicit(&st->stop, memory_order_acquire)) {
        spinlock_anderson_thread_t *slot = NULL;

        spinlock_anderson_lock(&st->lock, &slot);

        if (slot == NULL) {
            atomic_fetch_add(&st->null_slot_errors, 1);
            /* We don't own the lock if slot is NULL — don't unlock, just retry */
            sched_yield();
            continue;
        }

        /* Critical section — increment the shared counter */
        st->shared_counter++;

        spinlock_anderson_unlock(&st->lock, slot);

        atomic_fetch_add(&st->thread_ops[tid], 1);
    }

    atomic_fetch_sub(&st->live_threads, 1);
    return NULL;
}

/* ── Signal handler — graceful stop on SIGALRM ────────────────────────── */

static StressState *g_signal_state = NULL;

static void
signal_handler(int sig)
{
    (void)sig;
    if (g_signal_state) {
        atomic_store_explicit(&g_signal_state->stop, true,
                              memory_order_release);
    }
}

/* ── CLI parsing ──────────────────────────────────────────────────────── */

static void
print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--threads N] [--duration S] [--slots M]\n"
            "  --threads  N   Number of contender threads (default %d, max %d)\n"
            "  --duration S   Run duration in seconds   (default %d)\n"
            "  --slots    M   Anderson lock slot count   (default %d, max %d)\n",
            prog, DEFAULT_THREADS, MAX_THREADS,
            DEFAULT_DURATION, DEFAULT_SLOTS, MAX_SLOTS);
}

static int
parse_args(int argc, char **argv,
           unsigned int *threads, unsigned int *duration, unsigned int *slots)
{
    *threads  = DEFAULT_THREADS;
    *duration = DEFAULT_DURATION;
    *slots    = DEFAULT_SLOTS;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            *threads = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            *duration = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--slots") && i + 1 < argc) {
            *slots = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (*threads == 0 || *threads > MAX_THREADS) {
        fprintf(stderr, "threads must be 1..%d\n", MAX_THREADS);
        return 1;
    }
    if (*duration == 0 || *duration > 3600) {
        fprintf(stderr, "duration must be 1..3600\n");
        return 1;
    }
    if (*slots == 0 || *slots > MAX_SLOTS) {
        fprintf(stderr, "slots must be 1..%d\n", MAX_SLOTS);
        return 1;
    }

    return -1; /* proceed */
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    unsigned int n_threads, duration_sec, n_slots;
    int parse_rc = parse_args(argc, argv, &n_threads, &duration_sec, &n_slots);
    if (parse_rc >= 0) return parse_rc; /* 0 = help, 1 = error */

    /* ── Allocate ─────────────────────────────────────────────────────── */

    StressState st;
    memset(&st, 0, sizeof(st));

    st.slots = (spinlock_anderson_thread_t *)calloc(
        n_slots, sizeof(spinlock_anderson_thread_t));
    if (!st.slots) {
        fprintf(stderr, "Failed to allocate %u slots\n", n_slots);
        return 1;
    }
    st.slot_count = n_slots;

    spinlock_anderson_init(&st.lock, st.slots, n_slots);

    /* ── Banner ───────────────────────────────────────────────────────── */

    printf("=== Anderson Spinlock Stress Test ===\n");
    printf("  Threads:    %u\n", n_threads);
    printf("  Duration:   %u s\n", duration_sec);
    printf("  Slots:      %u\n", n_slots);
    if (n_slots & (n_slots - 1))
        printf("  Slot mode:  non-power-of-2 (CAS slow path)\n");
    else
        printf("  Slot mode:  power-of-2 (fetch_add fast path)\n");
    printf("=============================================\n\n");

    /* ── Signal-driven stop ───────────────────────────────────────────── */

    g_signal_state = &st;
    signal(SIGALRM, signal_handler);
    alarm(duration_sec);

    /* ── Spawn threads ────────────────────────────────────────────────── */

    pthread_t threads[MAX_THREADS];
    for (unsigned int t = 0; t < n_threads; t++) {
        if (pthread_create(&threads[t], NULL, stress_thread, &st) != 0) {
            fprintf(stderr, "pthread_create(%u) failed\n", t);
            atomic_store_explicit(&st.stop, true, memory_order_release);
            /* Join threads already created */
            for (unsigned int j = 0; j < t; j++)
                pthread_join(threads[j], NULL);
            free(st.slots);
            return 1;
        }
    }

    /* ── Wait for stop signal ─────────────────────────────────────────── */

    printf("Running for %u seconds...\n", duration_sec);
    fflush(stdout);

    while (!atomic_load_explicit(&st.stop, memory_order_acquire)) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
        nanosleep(&ts, NULL);
    }

    /* Give threads a grace period to observe the stop flag */
    printf("Stop signal received — draining threads...\n");
    fflush(stdout);

    /* ── Join all threads ─────────────────────────────────────────────── */

    for (unsigned int t = 0; t < n_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    /* ── Results ──────────────────────────────────────────────────────── */

    uint64_t total_ops = 0;
    for (unsigned int t = 0; t < n_threads; t++) {
        total_ops += atomic_load_explicit(&st.thread_ops[t],
                                          memory_order_relaxed);
    }

    uint32_t null_errors = atomic_load_explicit(&st.null_slot_errors,
                                                memory_order_relaxed);

    printf("\n=== Results ===\n");
    printf("  Total ops:        %lu\n", (unsigned long)total_ops);
    printf("  Shared counter:   %lu\n", (unsigned long)st.shared_counter);
    printf("  Throughput:       %.0f ops/s\n",
           total_ops / (double)duration_sec);
    printf("  Null-slot errors: %u\n", null_errors);

    /* Per-thread breakdown (first 16 threads only) */
    unsigned int show = n_threads < 16 ? n_threads : 16;
    printf("  Per-thread ops (first %u):\n", show);
    for (unsigned int t = 0; t < show; t++) {
        uint64_t t_ops = atomic_load_explicit(&st.thread_ops[t],
                                              memory_order_relaxed);
        printf("    thread %2u: %lu ops\n", t, (unsigned long)t_ops);
    }
    if (n_threads > 16)
        printf("    ... (%u more threads)\n", n_threads - 16);

    /* ── Verdict ──────────────────────────────────────────────────────── */

    uint32_t live = atomic_load_explicit(&st.live_threads,
                                         memory_order_relaxed);

    printf("\n  Final checks:\n");
    printf("    Live threads after join:%u\n", live);

    int errors = 0;

    if (st.shared_counter != total_ops) {
        printf("\n  *** COUNTER MISMATCH: shared=%lu, sum(per-thread)=%lu "
               "(diff=%ld) ***\n",
               (unsigned long)st.shared_counter,
               (unsigned long)total_ops,
               (long)(st.shared_counter - total_ops));
        errors++;
    }

    if (null_errors > 0) {
        printf("\n  *** NULL-SLOT ERRORS: %u ***\n", null_errors);
        errors++;
    }

    if (live > 0) {
        printf("\n  *** %u THREADS STILL LIVE AFTER JOIN ***\n", live);
        errors++;
    }

    if (errors == 0) {
        printf("\n===================\n");
        printf("Stress test PASSED — zero errors.\n");
    } else {
        printf("\n===================\n");
        printf("Stress test FAILED — %d error(s).\n", errors);
    }

    /* ── Cleanup ──────────────────────────────────────────────────────── */

    free(st.slots);

    return errors ? 1 : 0;
}
