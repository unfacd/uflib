/**
 * @file cdt_lockless_ringbuffer_stress.c
 * @brief Standalone multi-threaded stress test for the lock-free bounded ring
 *        buffer (LocklessRingBuffer — SPSC / MPSC / MPMC).
 *
 * Usage:
 *   cdt_lockless_ringbuffer_stress [--threads N] [--duration S] [--capacity C]
 *
 * Defaults: threads=8, duration=5, capacity=65536.
 *
 * For each mode, N producer threads push checksummed items in a tight loop
 * until the duration elapses; 1 (SPSC/MPSC) or N (MPMC) consumer threads pop
 * and validate.  Zero-error assertions:
 *
 *   - no item is corrupted (per-item checksum over the payload);
 *   - no item is delivered twice or dropped (consumed == produced exactly);
 *   - per-producer sequence numbers arrive strictly monotonically (+1)
 *     (single-consumer modes only);
 *   - the buffer drains to empty after shutdown.
 *
 * A non-zero error count indicates a correctness bug; the process exits 1.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <uflib/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

#define DEFAULT_THREADS  8
#define DEFAULT_DURATION 5
#define DEFAULT_CAPACITY 65536

/* ── Payload ────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t seq;
    uint64_t producer_id;
    uint64_t checksum;
} item_t;

static uint64_t
sChecksum(uint64_t seq, uint64_t producer_id)
{
    return (seq * 0x9E3779B97F4A7C15ULL) ^ (producer_id * 0xC2B2AE3D27D4EB4FULL);
}

/* ── Per-run shared state ───────────────────────────────────────────────── */

static _Atomic size_t g_produced;
static _Atomic size_t g_consumed;
static _Atomic bool   g_error;
static _Atomic bool   g_stop;

typedef struct {
    LocklessRingBuffer *ring;
    int                 id;
} prod_arg_t;

typedef struct {
    LocklessRingBuffer *ring;
    uint64_t           *next_seq;   /* per-producer FIFO (single-consumer only) */
} cons_arg_t;

/* ── Thread bodies ──────────────────────────────────────────────────────── */

static void *
sProducer(void *arg)
{
    prod_arg_t *a = (prod_arg_t *)arg;
    uint64_t    seq = 0;

    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        item_t it;
        it.seq         = seq;
        it.producer_id = (uint64_t)a->id;
        it.checksum    = sChecksum(it.seq, it.producer_id);

        if (LocklessRingBufferTryPush(a->ring, &it)) {
            atomic_fetch_add_explicit(&g_produced, 1, memory_order_relaxed);
            seq++;
        } else {
            sched_yield();
        }
    }
    return NULL;
}

static void *
sConsumer(void *arg)
{
    cons_arg_t *a = (cons_arg_t *)arg;

    for (;;) {
        item_t it;
        if (LocklessRingBufferTryPop(a->ring, &it)) {
            if (it.checksum != sChecksum(it.seq, it.producer_id)) {
                atomic_store_explicit(&g_error, true, memory_order_relaxed);
            }
            if (a->next_seq) {
                uint64_t expected = a->next_seq[it.producer_id];
                if (it.seq != expected) {
                    atomic_store_explicit(&g_error, true, memory_order_relaxed);
                }
                a->next_seq[it.producer_id] = expected + 1;
            }
            atomic_fetch_add_explicit(&g_consumed, 1, memory_order_relaxed);
        } else {
            bool   stopped  = atomic_load_explicit(&g_stop, memory_order_acquire);
            size_t produced = atomic_load_explicit(&g_produced, memory_order_acquire);
            size_t consumed = atomic_load_explicit(&g_consumed, memory_order_acquire);
            if (stopped && consumed >= produced) {
                break;   /* drained */
            }
            sched_yield();
        }
    }
    return NULL;
}

/* ── Per-mode runner ────────────────────────────────────────────────────── */

static int
sRunTest(const char *name, LocklessRingBufferMode mode,
         int nprod, int ncons, int duration_sec, size_t capacity)
{
    printf("\n=== %s  producers=%d consumers=%d duration=%ds capacity=%zu ===\n",
           name, nprod, ncons, duration_sec, capacity);

    atomic_store(&g_produced, 0);
    atomic_store(&g_consumed, 0);
    atomic_store(&g_error, false);
    atomic_store(&g_stop, false);

    LocklessRingBuffer *ring = LocklessRingBufferCreate(capacity, sizeof(item_t), mode);
    if (!ring) {
        fprintf(stderr, "  FAILED: create returned NULL\n");
        return 1;
    }

    pthread_t  *prods  = calloc((size_t)nprod, sizeof(pthread_t));
    pthread_t  *cons   = calloc((size_t)ncons, sizeof(pthread_t));
    prod_arg_t *pargs  = calloc((size_t)nprod, sizeof(prod_arg_t));
    cons_arg_t *cargs  = calloc((size_t)ncons, sizeof(cons_arg_t));
    uint64_t   *next_seq = (ncons == 1) ? calloc((size_t)nprod, sizeof(uint64_t)) : NULL;

    if (!prods || !cons || !pargs || !cargs || (ncons == 1 && !next_seq)) {
        fprintf(stderr, "  FAILED: thread-arg allocation failed\n");
        free(prods); free(cons); free(pargs); free(cargs); free(next_seq);
        LocklessRingBufferDestroy(ring);
        return 1;
    }

    for (int i = 0; i < nprod; i++) {
        pargs[i].ring = ring;
        pargs[i].id   = i;
        pthread_create(&prods[i], NULL, sProducer, &pargs[i]);
    }
    for (int i = 0; i < ncons; i++) {
        cargs[i].ring     = ring;
        cargs[i].next_seq = next_seq;
        pthread_create(&cons[i], NULL, sConsumer, &cargs[i]);
    }

    /* Let producers run for the requested duration, then stop and drain. */
    sleep((unsigned)duration_sec);
    atomic_store_explicit(&g_stop, true, memory_order_release);

    for (int i = 0; i < nprod; i++) pthread_join(prods[i], NULL);
    for (int i = 0; i < ncons; i++) pthread_join(cons[i], NULL);

    size_t produced = atomic_load(&g_produced);
    size_t consumed = atomic_load(&g_consumed);
    bool   error    = atomic_load(&g_error);

    bool drained = LocklessRingBufferEmpty(ring);

    printf("  Produced: %zu  Consumed: %zu  Errors: %s\n",
           produced, consumed, error ? "YES" : "no");

    int rc = 0;
    if (error) {
        fprintf(stderr, "  FAILED: corruption or ordering violation detected\n");
        rc = 1;
    }
    if (consumed != produced) {
        fprintf(stderr, "  FAILED: consumed (%zu) != produced (%zu)\n", consumed, produced);
        rc = 1;
    }
    if (!drained) {
        fprintf(stderr, "  FAILED: buffer did not drain to empty\n");
        rc = 1;
    }

    free(prods); free(cons); free(pargs); free(cargs); free(next_seq);
    LocklessRingBufferDestroy(ring);

    if (rc == 0) printf("  PASSED\n");
    return rc;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    int    threads      = DEFAULT_THREADS;
    int    duration_sec = DEFAULT_DURATION;
    size_t capacity     = DEFAULT_CAPACITY;

    static struct option long_opts[] = {
        { "threads",  required_argument, NULL, 't' },
        { "duration", required_argument, NULL, 'd' },
        { "capacity", required_argument, NULL, 'c' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "t:d:c:h", long_opts, NULL)) != -1) {
        switch (c) {
            case 't': threads      = atoi(optarg); break;
            case 'd': duration_sec = atoi(optarg); break;
            case 'c': capacity     = (size_t)atoll(optarg); break;
            case 'h':
            default:
                printf("Usage: %s [--threads N] [--duration S] [--capacity C]\n",
                       argv[0]);
                return (c == 'h') ? 0 : 1;
        }
    }

    if (threads < 1 || duration_sec < 1 || capacity < 1) {
        fprintf(stderr, "threads, duration, and capacity must be >= 1\n");
        return 1;
    }

    printf("Lock-free ring buffer — multi-threaded stress test\n");

    int rc = 0;
    rc |= sRunTest("SPSC", LOCKLESS_RINGBUF_MODE_SPSC, 1, 1, duration_sec, capacity);
    rc |= sRunTest("MPSC", LOCKLESS_RINGBUF_MODE_MPSC, threads, 1, duration_sec, capacity);
    rc |= sRunTest("MPMC", LOCKLESS_RINGBUF_MODE_MPMC, threads, threads, duration_sec, capacity);

    if (rc == 0) {
        printf("\nAll stress tests passed.\n");
    } else {
        printf("\nStress tests FAILED.\n");
    }
    return rc;
}
