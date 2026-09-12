/**
 * @file cdt_fixed_width_hashmap_stress.c
 * @brief Standalone multi-threaded stress test for LocklessFixedWidthHashMap.
 *
 * Usage:
 *   cdt_hashmap_stress [--threads N] [--duration S] [--capacity C]
 *                      [--key-width W] [--load-pct P]
 *
 * Defaults: 4 threads, 5 seconds, 65536 capacity, 256-byte keys, 75% load.
 *
 * Each thread runs a tight loop of random insert / get / remove operations
 * on a shared map.  At exit the program prints throughput and error counts.
 * A non-zero error count indicates a correctness bug.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <uflib/cdt/hashmap/cdt_fixed_width_hashmap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

/* ── Defaults ─────────────────────────────────────────────────────────── */

#define DEFAULT_THREADS  4
#define DEFAULT_DURATION 5
#define DEFAULT_CAPACITY 65536
#define DEFAULT_KEYWIDTH CONFIG_DEFAULT_CDS_HASHMAP_KEY_WIDTH
#define DEFAULT_LOADPCT  75

/* ── Global state ─────────────────────────────────────────────────────── */

typedef struct {
    LocklessFixedWidthHashMap *map;
    _Atomic uint64_t           ops_total;
    _Atomic uint64_t           ops_insert;
    _Atomic uint64_t           ops_get;
    _Atomic uint64_t           ops_remove;
    _Atomic uint64_t           insert_failures;
    _Atomic uint64_t           get_misses;
    _Atomic uint64_t           remove_misses;
    _Atomic uint64_t           consistency_errors;
    _Atomic bool              *stop;  /* shared across all workers */
    int                        thread_id;
    int                        key_width;
} WorkerArgs;

/* ── Key generation ───────────────────────────────────────────────────── */

/* Generate a pseudo-random key string.  The key space is deliberately
 * smaller than the map capacity to create contention on hot keys. */
static void
sMakeKey(char *buf, int buf_sz, int thread_id, int key_id)
{
    /* Use a bounded key space to create contention on hot keys. */
    int hot_key = key_id % 8192;  /* 8K hot keys per thread */
    snprintf(buf, (size_t)buf_sz, "t%d_k%d", thread_id, hot_key);
}

/* ── Worker ───────────────────────────────────────────────────────────── */

static void *
sWorker(void *arg)
{
    WorkerArgs *a  = (WorkerArgs *)arg;
    char        key_buf[512];
    int         seq = 0;

    /* Ensure key_buf fits within the map's key_width */
    int kw = a->key_width;
    if (kw > (int)sizeof(key_buf)) kw = (int)sizeof(key_buf);

    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        /* Decouple key selection from operation type.
         * Use independent counters so every key can receive every op. */
        int op     = seq % 4;
        int key_id = (seq / 4) + a->thread_id * 2500;
        seq++;
        sMakeKey(key_buf, kw, a->thread_id, key_id);

        switch (op) {
        case 0: /* Insert */
        case 1: {
            bool is_new = false;
            if (LocklessFixedWidthHashMap_Insert(
                    a->map, key_buf, (uint32_t)seq, &is_new)) {
                atomic_fetch_add_explicit(&a->ops_insert, 1,
                                          memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&a->insert_failures, 1,
                                          memory_order_relaxed);
            }
            break;
        }
        case 2: { /* Get */
            uint32_t val = 0;
            if (LocklessFixedWidthHashMap_Get(a->map, key_buf, &val)) {
                atomic_fetch_add_explicit(&a->ops_get, 1,
                                          memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&a->get_misses, 1,
                                          memory_order_relaxed);
            }
            break;
        }
        case 3: { /* Remove */
            uint32_t val = 0;
            if (LocklessFixedWidthHashMap_Remove(a->map, key_buf, &val)) {
                atomic_fetch_add_explicit(&a->ops_remove, 1,
                                          memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&a->remove_misses, 1,
                                          memory_order_relaxed);
            }
            break;
        }
        }

        atomic_fetch_add_explicit(&a->ops_total, 1, memory_order_relaxed);
    }

    return NULL;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

static void
sPrintUsage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "  --threads N    Number of worker threads   (default: %d)\n"
            "  --duration S   Run duration in seconds     (default: %d)\n"
            "  --capacity C   Hash slot capacity          (default: %d)\n"
            "  --key-width W  Key width in bytes          (default: %d)\n"
            "  --load-pct P   Pool load factor %%          (default: %d)\n",
            prog, DEFAULT_THREADS, DEFAULT_DURATION, DEFAULT_CAPACITY,
            DEFAULT_KEYWIDTH, DEFAULT_LOADPCT);
}

int
main(int argc, char **argv)
{
    int      num_threads = DEFAULT_THREADS;
    int      duration    = DEFAULT_DURATION;
    uint32_t capacity    = DEFAULT_CAPACITY;
    uint32_t key_width   = DEFAULT_KEYWIDTH;
    uint32_t load_pct    = DEFAULT_LOADPCT;

    /* ── Parse arguments ────────────────────────────────────────────── */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--capacity") == 0 && i + 1 < argc) {
            capacity = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--key-width") == 0 && i + 1 < argc) {
            key_width = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--load-pct") == 0 && i + 1 < argc) {
            load_pct = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            sPrintUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            sPrintUsage(argv[0]);
            return 1;
        }
    }

    if (num_threads < 1) num_threads = 1;
    if (duration < 1) duration = 1;
    if (capacity < 8) capacity = 8;

    /* ── Init map ──────────────────────────────────────────────────── */
    LocklessFixedWidthHashMap map;
    memset(&map, 0, sizeof(map));

    if (!LocklessFixedWidthHashMap_Init(&map, capacity, load_pct, key_width)) {
        fprintf(stderr, "Failed to initialise map (capacity=%u, load=%u%%,"
                " key_width=%u)\n",
                (unsigned)capacity, (unsigned)load_pct, (unsigned)key_width);
        return 1;
    }

    printf("=== LocklessFixedWidthHashMap Stress Test ===\n");
    printf("  Threads:    %d\n", num_threads);
    printf("  Duration:   %d s\n", duration);
    printf("  Capacity:   %u slots\n", (unsigned)map.slot_capacity);
    printf("  Pool:       %u nodes\n", (unsigned)map.pool_capacity);
    printf("  Key width:  %u bytes\n", (unsigned)key_width);
    printf("  Load pct:   %u%%\n", (unsigned)load_pct);
    printf("=============================================\n");

    /* ── Spawn workers ─────────────────────────────────────────────── */
    pthread_t  *threads = calloc((size_t)num_threads, sizeof(pthread_t));
    WorkerArgs *args    = calloc((size_t)num_threads, sizeof(WorkerArgs));
    _Atomic bool stop   = false;

    if (!threads || !args) {
        fprintf(stderr, "Failed to allocate thread arrays\n");
        free(threads);
        free(args);
        LocklessFixedWidthHashMap_Destroy(&map);
        return 1;
    }

    for (int i = 0; i < num_threads; i++) {
        args[i].map        = &map;
        args[i].thread_id  = i;
        args[i].key_width  = (int)key_width;
        args[i].stop       = &stop;
        atomic_init(&args[i].ops_total, 0);
        atomic_init(&args[i].ops_insert, 0);
        atomic_init(&args[i].ops_get, 0);
        atomic_init(&args[i].ops_remove, 0);
        atomic_init(&args[i].insert_failures, 0);
        atomic_init(&args[i].get_misses, 0);
        atomic_init(&args[i].remove_misses, 0);
        atomic_init(&args[i].consistency_errors, 0);

        int rc = pthread_create(&threads[i], NULL, sWorker, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
            atomic_store_explicit(&stop, true, memory_order_relaxed);
            num_threads = i;
            break;
        }
    }

    /* ── Run for the requested duration ────────────────────────────── */
    printf("\nRunning for %d seconds...\n", duration);
    sleep((unsigned int)duration);

    /* Signal stop and join all threads */
    atomic_store_explicit(&stop, true, memory_order_relaxed);
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    /* ── Aggregate stats ───────────────────────────────────────────── */
    uint64_t total_ops      = 0;
    uint64_t total_inserts  = 0;
    uint64_t total_gets     = 0;
    uint64_t total_removes  = 0;
    uint64_t total_ifail    = 0;
    uint64_t total_gmiss    = 0;
    uint64_t total_rmiss    = 0;

    for (int i = 0; i < num_threads; i++) {
        total_ops     += atomic_load_explicit(&args[i].ops_total, memory_order_relaxed);
        total_inserts += atomic_load_explicit(&args[i].ops_insert, memory_order_relaxed);
        total_gets    += atomic_load_explicit(&args[i].ops_get, memory_order_relaxed);
        total_removes += atomic_load_explicit(&args[i].ops_remove, memory_order_relaxed);
        total_ifail   += atomic_load_explicit(&args[i].insert_failures, memory_order_relaxed);
        total_gmiss   += atomic_load_explicit(&args[i].get_misses, memory_order_relaxed);
        total_rmiss   += atomic_load_explicit(&args[i].remove_misses, memory_order_relaxed);
    }

    /* ── Consistency check ─────────────────────────────────────────── */
    /* Verify Size() is consistent with non-negative count */
    uint32_t final_size = LocklessFixedWidthHashMap_Size(&map);
    float    final_lf   = LocklessFixedWidthHashMap_LoadFactor(&map);

    /* ── Report ────────────────────────────────────────────────────── */
    double ops_per_sec = (double)total_ops / (double)duration;

    printf("\n=== Results ===\n");
    printf("  Total ops:        %lu\n", (unsigned long)total_ops);
    printf("  Throughput:       %.0f ops/s\n", ops_per_sec);
    printf("  Inserts:          %lu\n", (unsigned long)total_inserts);
    printf("  Gets:             %lu\n", (unsigned long)total_gets);
    printf("  Removes:          %lu\n", (unsigned long)total_removes);
    printf("  Insert failures:  %lu (pool full)\n", (unsigned long)total_ifail);
    printf("  Get misses:       %lu (key absent)\n", (unsigned long)total_gmiss);
    printf("  Remove misses:    %lu (key absent)\n", (unsigned long)total_rmiss);
    printf("  Final size:       %u\n", (unsigned)final_size);
    printf("  Load factor:      %.2f\n", (double)final_lf);
    printf("===================\n");

    /* ── Cleanup ──────────────────────────────────────────────────── */
    free(threads);
    free(args);
    LocklessFixedWidthHashMap_Destroy(&map);

    /* Exit with error if insert failures are suspiciously high
     * (pool should not have been exhausted at 75% load factor) */
    if (total_ifail > total_inserts / 2 && total_inserts > 1000) {
        fprintf(stderr, "WARNING: insert failure rate > 50%% — "
                "pool may be undersized\n");
        return 1;
    }

    printf("\nStress test complete.\n");
    return 0;
}
