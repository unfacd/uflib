/**
 * @file scheduled_jobs_mt_stress.c
 * @brief Rigorous multi-threaded stress test for the V1 scheduled_jobs module.
 *
 * Unlike scheduled_jobs_stress.c (which only counts ops), this harness checks
 * two invariants after the concurrent phase:
 *
 *   1. No job is lost   — inserted == removed + drained.
 *   2. Order is sound   — the final drain yields non-decreasing fire-times.
 *
 * Each insert is given a unique, strictly-increasing fire time from a shared
 * atomic sequence, so (2) is violated by any lost, duplicated, or mis-ordered
 * entry.  The store's own spinlock serialises the underlying heap.
 *
 * Usage:
 *   scheduled_jobs_mt_stress --threads 8 --ops 50000
 *
 * Exit 0 on pass, non-zero on any invariant violation.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>

#include <uflib/scheduled_jobs/scheduled_jobs.h>

/* ── Fake clock (constant) ──────────────────────────────────────────── */

static long long sFakeGetTime(void)
{
    return 0;
}

/* ── Job type (single, shared) ──────────────────────────────────────── */

static ScheduledJobType s_job_type;

static int sFakeOnRun(void *ctx, void *data)
{
    (void)ctx;
    (void)data;
    return 0;
}

/* ── GetScheduledJobsStore (consumer-defined) ───────────────────────── */

static ScheduledJobs s_store;

ScheduledJobs *GetScheduledJobsStore(void)
{
    return &s_store;
}

/* ── Shared state ───────────────────────────────────────────────────── */

static _Atomic long long s_seq      = 0;  /* unique fire-time generator */
static _Atomic long long s_inserted = 0;
static _Atomic long long s_removed  = 0;

static int s_ops_per_thread = 0;

/* ── Worker ─────────────────────────────────────────────────────────── */

static void *sWorker(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;

    for (int i = 0; i < s_ops_per_thread; i++) {
        seed = seed * 1103515245u + 12345u;
        int do_insert = ((seed >> 16) % 100) < 70;

        if (do_insert) {
            ScheduledJob *job = calloc(1, sizeof(*job));
            if (!job)
                continue;
            job->job_type_ptr     = &s_job_type;
            job->when_to_schedule = atomic_fetch_add(&s_seq, 1) + 1;  /* > 0 */
            InsertScheduledJob(&s_store, job);
            atomic_fetch_add(&s_inserted, 1);
        } else {
            ScheduledJobContext ctx;
            memset(&ctx, 0, sizeof(ctx));
            if (GetRemScheduledJob(&s_store, LOCK_HINT_NONE, &ctx)) {
                atomic_fetch_add(&s_removed, 1);
                free(ctx.scheduled_job_ptr);
            }
        }
    }
    return NULL;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int num_threads = 8;
    s_ops_per_thread = 50000;

    static struct option long_opts[] = {
        {"threads", required_argument, 0, 't'},
        {"ops",     required_argument, 0, 'o'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:o:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't': num_threads = atoi(optarg); break;
        case 'o': s_ops_per_thread = atoi(optarg); break;
        default:
            fprintf(stderr, "Usage: %s [--threads N] [--ops N]\n", argv[0]);
            return 1;
        }
    }

    if (num_threads < 1 || s_ops_per_thread < 0) {
        fprintf(stderr, "Invalid args\n");
        return 1;
    }

    printf("ScheduledJobs MT stress: threads=%d ops/thread=%d\n",
           num_threads, s_ops_per_thread);

    InitScheduledJobsStore(&s_store, 16);
    memset(&s_job_type, 0, sizeof(s_job_type));
    s_job_type.type_name       = "mt_stress_job";
    s_job_type.frequency_mode  = PERIODIC;
    s_job_type.frequency       = 1000;
    s_job_type.callbacks.on_get_time = sFakeGetTime;
    s_job_type.callbacks.on_run      = sFakeOnRun;
    RegisterScheduledJobType(&s_store, &s_job_type);

    pthread_t *threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!threads) { perror("calloc"); return 2; }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, sWorker, (void *)(uintptr_t)(i + 1)))
            { perror("pthread_create"); return 2; }
    }
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    /* Drain and verify order + count. */
    long long prev = -1;
    int first = 1;
    long long drained = 0;
    int order_ok = 1;
    for (;;) {
        ScheduledJobContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        if (!GetRemScheduledJob(&s_store, LOCK_HINT_NONE, &ctx))
            break;
        if (!first && ctx.time_key < prev) {
            fprintf(stderr, "ORDER VIOLATION: %lld after %lld\n",
                    ctx.time_key, prev);
            order_ok = 0;
        }
        first = 0;
        prev = ctx.time_key;
        drained++;
        free(ctx.scheduled_job_ptr);
    }

    long long inserted = atomic_load(&s_inserted);
    long long removed  = atomic_load(&s_removed);

    size_t final_size = GetScheduleJobsSetsize(&s_store, LOCK_HINT_NONE);

    printf("inserted=%lld removed=%lld drained=%lld final_size=%zu order=%s\n",
           inserted, removed, drained, final_size, order_ok ? "ok" : "VIOLATED");

    int rc = 0;
    if (!order_ok) rc = 1;
    if (inserted != removed + drained) {
        fprintf(stderr, "LOST JOBS: inserted %lld != removed %lld + drained %lld\n",
                inserted, removed, drained);
        rc = 1;
    }
    if (final_size != 0) {
        fprintf(stderr, "NON-EMPTY STORE: final_size=%zu\n", final_size);
        rc = 1;
    }

    free(threads);
    DestructScheduledJobs(&s_store);

    printf(rc ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return rc;
}
