/**
 * @file cdt_lockless_lru_stress.c
 * @brief Standalone multi-threaded stress test + microbench for LocklessLru.
 *
 * Usage:
 *   cdt_lockless_lru_stress [--threads N] [--duration S] [--capacity C]
 *                           [--keys-mult M] [--seed S] [--bench]
 *
 * Defaults: 8 threads, 3 seconds, 4096 capacity, 4× key space.
 *
 * Workload is 40% SetEx / 40% GetRef / 20% Remove over a random key space.
 * Every payload carries a MAGIC guard; every displaced / hit / removed
 * pointer is checked for it, and SetEx statuses are classified (inserted /
 * replaced / evicted / table-full).  Generation snapshots are re-validated
 * via RefStillValid to detect slot reuse (counted, informational — reuse is
 * allowed, corruption is not).  Exit status is 0 on PASS, non-zero on any
 * corruption or a final size that exceeds the physical slot table.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <getopt.h>
#include <unistd.h>

#include <uflib/cdt/lockless_lru/cdt_lockless_lru.h>

#define DEFAULT_THREADS  8
#define DEFAULT_DURATION 3
#define DEFAULT_CAPACITY 4096
#define DEFAULT_KEY_MULT 4
#define DEFAULT_SEED     0x9e3779b9u

typedef struct {
    uint64_t key;
    long     value;
    uint32_t magic;
} TestPayload;

#define MAGIC 0xA5C0FFEEU

typedef struct {
    LocklessLru       *lru;
    TestPayload       *pool;
    size_t             key_space;
    unsigned           base_seed;
    _Atomic(bool)      stop;
    int                duration_s;
    _Atomic(uint64_t)  total_ops;
    _Atomic(uint64_t)  inserts;
    _Atomic(uint64_t)  replaces;
    _Atomic(uint64_t)  evicts;
    _Atomic(uint64_t)  fulls;
    _Atomic(uint64_t)  gets;
    _Atomic(uint64_t)  get_hits;
    _Atomic(uint64_t)  removes;
    _Atomic(uint64_t)  corrupt;
    _Atomic(uint64_t)  gen_mismatch;
} StressState;

static StressState *g_st = NULL;

static void
sSig(int sig)
{
    (void)sig;
    if (g_st) atomic_store_explicit(&g_st->stop, true, memory_order_relaxed);
}

static double
sNow(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void *
worker(void *arg)
{
    StressState *st = (StressState *)arg;
    /* Derive a per-thread seed from the base seed + the thread's address. */
    unsigned seed = st->base_seed ^ (unsigned)(uintptr_t)pthread_self();
    uint64_t ops = 0, ins = 0, repl = 0, ev = 0, full = 0, gets = 0, hits = 0;
    uint64_t rems = 0, corrupt = 0, gmm = 0;

    while (!atomic_load_explicit(&st->stop, memory_order_relaxed)) {
        uint64_t key = (uint64_t)(rand_r(&seed) % (int)st->key_space);
        int op = rand_r(&seed) % 10; /* 4 set / 4 get / 2 remove */

        if (op < 4) {
            LocklessLruSetResult r = LocklessLruSetEx(st->lru, key,
                                                      (LruClientData *)&st->pool[key]);
            if (r.status == LOCKLESS_LRU_SET_INSERTED) ins++;
            else if (r.status == LOCKLESS_LRU_SET_REPLACED) repl++;
            else if (r.status == LOCKLESS_LRU_SET_EVICTED) ev++;
            else full++;
            if (r.displaced) {
                TestPayload *p = (TestPayload *)r.displaced;
                if (p->magic != MAGIC) corrupt++;
            }
        } else if (op < 8) {
            LocklessLruRef ref = LocklessLruGetRef(st->lru, key);
            gets++;
            if (ref.data) {
                hits++;
                TestPayload *p = (TestPayload *)ref.data;
                if (p->magic != MAGIC || p->key != key) corrupt++;
                if (!LocklessLruRefStillValid(st->lru, key, ref.gen))
                    gmm++; /* reuse is allowed; count only for stats */
            }
        } else {
            LruClientData *rm = LocklessLruRemove(st->lru, key);
            rems++;
            if (rm) {
                TestPayload *p = (TestPayload *)rm;
                if (p->magic != MAGIC) corrupt++;
            }
        }
        ops++;
    }

    atomic_fetch_add_explicit(&st->total_ops, ops, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->inserts, ins, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->replaces, repl, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->evicts, ev, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->fulls, full, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->gets, gets, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->get_hits, hits, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->removes, rems, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->corrupt, corrupt, memory_order_relaxed);
    atomic_fetch_add_explicit(&st->gen_mismatch, gmm, memory_order_relaxed);
    return NULL;
}

static int
run_stress(int threads, int duration, int capacity, size_t key_space, unsigned base_seed)
{
    LocklessLruConfig cfg = { .capacity_hint = (size_t)capacity };
    LocklessLru *lru = LocklessLruCreate(&cfg);
    if (!lru) {
        fprintf(stderr, "Create failed\n");
        return 1;
    }

    TestPayload *pool = calloc(key_space, sizeof(*pool));
    if (!pool) {
        LocklessLruDestroy(lru);
        return 1;
    }
    for (size_t i = 0; i < key_space; i++) {
        pool[i].key = (uint64_t)i;
        pool[i].value = (long)i;
        pool[i].magic = MAGIC;
    }

    StressState st = {
        .lru = lru,
        .pool = pool,
        .key_space = key_space,
        .base_seed = base_seed,
        .stop = false,
        .duration_s = duration,
    };
    g_st = &st;
    signal(SIGINT, sSig);
    signal(SIGALRM, sSig);

    printf("=== LocklessLru stress ===\n");
    printf("  threads=%d duration=%ds capacity_hint=%d phys=%zu keys=%zu seed=%u\n",
           threads, duration, capacity, LocklessLruCapacity(lru), key_space, base_seed);

    pthread_t *th = calloc((size_t)threads, sizeof(*th));
    if (!th) {
        LocklessLruDestroy(lru);
        free(pool);
        g_st = NULL;
        return 1;
    }

    double t0 = sNow();
    for (int i = 0; i < threads; i++)
        pthread_create(&th[i], NULL, worker, &st);
    alarm((unsigned)duration);
    for (int i = 0; i < threads; i++)
        pthread_join(th[i], NULL);
    double elapsed = sNow() - t0;

    uint64_t total = atomic_load(&st.total_ops);
    uint64_t corrupt = atomic_load(&st.corrupt);
    size_t final_sz = LocklessLruSize(lru);
    size_t phys = LocklessLruCapacity(lru);

    printf("  elapsed          %.3f s\n", elapsed);
    printf("  total ops        %lu\n", (unsigned long)total);
    printf("  throughput       %.0f ops/s\n", (double)total / elapsed);
    printf("  inserts          %lu\n", (unsigned long)atomic_load(&st.inserts));
    printf("  replaces         %lu\n", (unsigned long)atomic_load(&st.replaces));
    printf("  evictions        %lu\n", (unsigned long)atomic_load(&st.evicts));
    printf("  table-full       %lu\n", (unsigned long)atomic_load(&st.fulls));
    printf("  gets             %lu (hits %lu)\n",
           (unsigned long)atomic_load(&st.gets),
           (unsigned long)atomic_load(&st.get_hits));
    printf("  removes          %lu\n", (unsigned long)atomic_load(&st.removes));
    printf("  payload corrupt  %lu\n", (unsigned long)corrupt);
    printf("  gen stale        %lu (informational)\n",
           (unsigned long)atomic_load(&st.gen_mismatch));
    printf("  final size       %zu / hint %d / phys %zu\n", final_sz, capacity, phys);

    int errors = 0;
    if (corrupt) {
        fprintf(stderr, "FAIL: payload corruption\n");
        errors++;
    }
    if (final_sz > phys) {
        fprintf(stderr, "FAIL: size %zu > physical %zu\n", final_sz, phys);
        errors++;
    }

    printf(errors ? "  RESULT FAIL (%d)\n" : "  RESULT PASS\n", errors);

    LocklessLruDestroy(lru);
    free(pool);
    free(th);
    g_st = NULL;
    return errors ? 1 : 0;
}

/* Sequential microbench: tight Get/Set on a warm cache. */
static int
run_bench(int capacity)
{
    LocklessLruConfig cfg = { .capacity_hint = (size_t)capacity };
    LocklessLru *lru = LocklessLruCreate(&cfg);
    if (!lru) return 1;

    size_t n = (size_t)capacity;
    TestPayload *pool = calloc(n, sizeof(*pool));
    for (size_t i = 0; i < n; i++) {
        pool[i].key = i;
        pool[i].value = (long)i;
        pool[i].magic = MAGIC;
        LocklessLruSet(lru, i, (LruClientData *)&pool[i]);
    }

    const int iters = 200000;
    double t0 = sNow();
    volatile uintptr_t sink = 0;
    for (int i = 0; i < iters; i++) {
        LruClientData *p = LocklessLruGet(lru, (uint64_t)(i % (int)n));
        sink ^= (uintptr_t)p;
    }
    double tg = sNow() - t0;

    t0 = sNow();
    for (int i = 0; i < iters; i++) {
        uint64_t k = (uint64_t)(i % (int)n);
        LocklessLruSetEx(lru, k, (LruClientData *)&pool[k]);
    }
    double ts = sNow() - t0;

    printf("=== LocklessLru microbench (single thread, cap=%d) ===\n", capacity);
    printf("  Get    %.0f ops/s  (%.2f ns/op)\n",
           (double)iters / tg, tg * 1e9 / (double)iters);
    printf("  SetEx  %.0f ops/s  (%.2f ns/op)\n",
           (double)iters / ts, ts * 1e9 / (double)iters);
    printf("  sink   %lu\n", (unsigned long)sink);

    LocklessLruDestroy(lru);
    free(pool);
    return 0;
}

int
main(int argc, char **argv)
{
    int threads = DEFAULT_THREADS;
    int duration = DEFAULT_DURATION;
    int capacity = DEFAULT_CAPACITY;
    int bench = 0;
    int key_mult = DEFAULT_KEY_MULT;
    unsigned base_seed = DEFAULT_SEED;

    static struct option long_opts[] = {
        {"threads",   required_argument, 0, 't'},
        {"duration",  required_argument, 0, 'd'},
        {"capacity",  required_argument, 0, 'c'},
        {"keys-mult", required_argument, 0, 'k'},
        {"seed",      required_argument, 0, 's'},
        {"bench",     no_argument,       0, 'b'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:d:c:k:s:bh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't': threads = atoi(optarg); break;
        case 'd': duration = atoi(optarg); break;
        case 'c': capacity = atoi(optarg); break;
        case 'k': key_mult = atoi(optarg); break;
        case 's': base_seed = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'b': bench = 1; break;
        default:
            fprintf(stderr,
                    "Usage: %s [--threads N] [--duration S] [--capacity C] "
                    "[--keys-mult M] [--seed S] [--bench]\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (threads < 1) threads = 1;
    if (duration < 1) duration = 1;
    if (capacity < 8) capacity = 8;
    if (key_mult < 1) key_mult = 1;

    int rc = 0;
    if (bench)
        rc |= run_bench(capacity);
    rc |= run_stress(threads, duration, capacity, (size_t)capacity * (size_t)key_mult,
                     base_seed);
    return rc;
}
