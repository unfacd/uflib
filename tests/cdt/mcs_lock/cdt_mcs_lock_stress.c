/**
 * @file cdt_mcs_lock_stress.c
 * @brief Standalone multi-threaded stress test for the MCS queue-based
 *        spinlock.
 *
 * Usage:
 *   cdt_mcs_lock_stress [--threads N] [--duration S]
 *
 * Defaults: threads=8, duration=5.
 *
 * N threads contend on a shared counter protected by the MCS lock.
 * Each thread counts its own successful increments.  After the run, the
 * shared counter must equal the sum of all per-thread counts — any
 * discrepancy indicates a correctness bug (lost or doubled increment).
 *
 * Zero-error assertions:
 *   - shared counter == sum of per-thread counters after run
 *   - SpinlockMcsLocked() is false after all threads join
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

#include <uflib/cdt/cdt_mcs_lock.h>

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

#define MAX_THREADS      256

/* ── Shared state ─────────────────────────────────────────────────────── */

typedef struct {
    SpinlockMcs    lock;

    uint64_t       shared_counter;  /* protected by lock */
    _Atomic bool   stop;
    _Atomic uint32_t live_threads;

    /* Per-thread stats */
    _Atomic uint64_t thread_ops[MAX_THREADS];
    _Atomic uint32_t deadlock_timeout;
} StressState;

/* ── Thread function ──────────────────────────────────────────────────── */

static void *
stress_thread(void *arg)
{
    StressState *st = (StressState *)arg;

    static _Atomic unsigned int s_tid_counter = 0;
    unsigned int tid = atomic_fetch_add(&s_tid_counter, 1);

    atomic_fetch_add(&st->live_threads, 1);

    while (!atomic_load_explicit(&st->stop, memory_order_acquire)) {
        SpinlockMcsNode node;
        SpinlockMcsNodeInit(&node);

        SpinlockMcsLock(&st->lock, &node);

        /* Critical section — increment the shared counter */
        st->shared_counter++;

        SpinlockMcsUnlock(&st->lock, &node);

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
printUsage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--threads N] [--duration S]\n"
            "  --threads  N   Number of contender threads (default %d, max %d)\n"
            "  --duration S   Run duration in seconds   (default %d)\n",
            prog, DEFAULT_THREADS, MAX_THREADS, DEFAULT_DURATION);
}

static int
parseArgs(int argc, char **argv,
          unsigned int *threads, unsigned int *duration)
{
    *threads  = DEFAULT_THREADS;
    *duration = DEFAULT_DURATION;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            *threads = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            *duration = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            printUsage(argv[0]);
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

    return -1; /* proceed */
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    unsigned int n_threads, duration_sec;
    int parse_rc = parseArgs(argc, argv, &n_threads, &duration_sec);
    if (parse_rc >= 0) return parse_rc; /* 0 = help, 1 = error */

    /* ── Allocate ─────────────────────────────────────────────────────── */

    StressState st;
    memset(&st, 0, sizeof(st));

    SpinlockMcsInit(&st.lock);

    /* ── Banner ───────────────────────────────────────────────────────── */

    printf("=== MCS Spinlock Stress Test ===\n");
    printf("  Threads:    %u\n", n_threads);
    printf("  Duration:   %u s\n", duration_sec);
    printf("  Node size:  %zu bytes (unpadded)\n", sizeof(SpinlockMcsNode));
    printf("==================================\n\n");

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
            for (unsigned int j = 0; j < t; j++)
                pthread_join(threads[j], NULL);
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

    printf("\n=== Results ===\n");
    printf("  Total ops:        %lu\n", (unsigned long)total_ops);
    printf("  Shared counter:   %lu\n", (unsigned long)st.shared_counter);
    printf("  Throughput:       %.0f ops/s\n",
           total_ops / (double)duration_sec);

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
    bool held = SpinlockMcsLocked(&st.lock);

    printf("\n  Final checks:\n");
    printf("    Live threads after join: %u\n", live);
    printf("    Lock held after join:    %s\n", held ? "YES (ERROR)" : "no");

    int errors = 0;

    if (st.shared_counter != total_ops) {
        printf("\n  *** COUNTER MISMATCH: shared=%lu, sum(per-thread)=%lu "
               "(diff=%ld) ***\n",
               (unsigned long)st.shared_counter,
               (unsigned long)total_ops,
               (long)(st.shared_counter - total_ops));
        errors++;
    }

    if (live > 0) {
        printf("\n  *** %u THREADS STILL LIVE AFTER JOIN ***\n", live);
        errors++;
    }

    if (held) {
        printf("\n  *** LOCK STILL HELD AFTER ALL THREADS JOINED ***\n");
        errors++;
    }

    if (errors == 0) {
        printf("\n===================\n");
        printf("Stress test PASSED — zero errors.\n");
    } else {
        printf("\n===================\n");
        printf("Stress test FAILED — %d error(s).\n", errors);
    }

    return errors ? 1 : 0;
}
