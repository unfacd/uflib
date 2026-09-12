/**
 * @file cdt_lamport_queue_stress.c
 * @brief Standalone SPSC stress test for LamportQueue (LocklessSpscQueue).
 *
 * Usage:
 *   cdt_lamport_queue_stress [--duration S] [--capacity C]
 *
 * Defaults: duration=5, capacity=65536.
 *
 * Exactly two threads: one producer, one consumer.  The producer pushes
 * uintptr_t-cast sequence numbers; the consumer pops and validates them
 * for gaps or corruption.  A non-zero error count indicates a correctness
 * bug.
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

#include <uflib/cdt/cdt_lamport_queue.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

/* ── Defaults ─────────────────────────────────────────────────────────── */

#define DEFAULT_DURATION 5
#define DEFAULT_CAPACITY 65536

/* ── Global state ─────────────────────────────────────────────────────── */

typedef struct {
    LocklessSpscQueue      *queue;
    _Atomic uint64_t        ops_produced;
    _Atomic uint64_t        ops_consumed;
    _Atomic uint64_t        sequence_errors;
    _Atomic bool           *stop;
} WorkerArgs;

/* ── Worker functions ─────────────────────────────────────────────────── */

static void *
sProducer(void *arg)
{
    WorkerArgs *a = (WorkerArgs *)arg;
    uint64_t    seq = 0;

    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        if (LamportQueuePush(a->queue, AS_QUEUE_CLIENT_DATA((uintptr_t)(seq + 1)))) {
            atomic_fetch_add_explicit(&a->ops_produced, 1,
                                      memory_order_relaxed);
            seq++;
        }
        /* else: queue full — spin and retry */
    }

    return NULL;
}

static void *
sConsumer(void *arg)
{
    WorkerArgs *a = (WorkerArgs *)arg;
    uint64_t    expected = 1;

    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        QueueClientData *p = NULL;
        if (LamportQueuePop(a->queue, &p)) {
            uint64_t id = (uint64_t)(uintptr_t)p;
            if (id != expected) {
                /*
                 * Non-monotonic or gapped sequence — correctness violation.
                 * Report once (first error) to avoid flooding stderr.
                 */
                uint64_t prev_errs = atomic_fetch_add_explicit(
                    &a->sequence_errors, 1, memory_order_relaxed);
                if (prev_errs == 0) {
                    fprintf(stderr,
                            "SEQUENCE ERROR: expected %lu, got %lu (delta=%ld)\n",
                            (unsigned long)expected, (unsigned long)id,
                            (long)(id - expected));
                }
                expected = id + 1;
            } else {
                expected = id + 1;
            }
            atomic_fetch_add_explicit(&a->ops_consumed, 1,
                                      memory_order_relaxed);
        }
        /* else: queue empty — spin and retry */
    }

    return NULL;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

static void
sPrintUsage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "  --duration S   Run duration in seconds     (default: %d)\n"
            "  --capacity C   Queue slot capacity          (default: %d)\n",
            prog, DEFAULT_DURATION, DEFAULT_CAPACITY);
}

int
main(int argc, char **argv)
{
    int      duration = DEFAULT_DURATION;
    uint32_t capacity = DEFAULT_CAPACITY;

    /* ── Parse arguments ────────────────────────────────────────────── */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--capacity") == 0 && i + 1 < argc) {
            capacity = (uint32_t)atoi(argv[++i]);
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

    if (duration < 1) duration = 1;
    if (capacity < 2) capacity = 2;

    /* ── Init queue ──────────────────────────────────────────────────── */
    LocklessSpscQueue queue;
    memset(&queue, 0, sizeof(queue));
    LamportQueueInit(&queue, NULL, (size_t)capacity);

    printf("=== LocklessSpscQueue (Lamport SPSC) Stress Test ===\n");
    printf("  Threads:    2 (1 producer + 1 consumer)\n");
    printf("  Duration:   %d s\n", duration);
    printf("  Capacity:   %u slots\n", (unsigned)capacity);
    printf("=====================================================\n");

    /* ── Spawn threads ──────────────────────────────────────────────── */
    _Atomic bool stop   = false;
    WorkerArgs  p_args = { .queue = &queue, .stop = &stop };
    WorkerArgs  c_args = { .queue = &queue, .stop = &stop };
    pthread_t   producer_th, consumer_th;

    atomic_init(&p_args.ops_produced, 0);
    atomic_init(&p_args.ops_consumed, 0);
    atomic_init(&p_args.sequence_errors, 0);
    atomic_init(&c_args.ops_produced, 0);
    atomic_init(&c_args.ops_consumed, 0);
    atomic_init(&c_args.sequence_errors, 0);

    int rc = pthread_create(&producer_th, NULL, sProducer, &p_args);
    if (rc != 0) {
        fprintf(stderr, "pthread_create (producer) failed: %s\n",
                strerror(rc));
        LamportQueueDestroy(&queue);
        return 1;
    }

    rc = pthread_create(&consumer_th, NULL, sConsumer, &c_args);
    if (rc != 0) {
        fprintf(stderr, "pthread_create (consumer) failed: %s\n",
                strerror(rc));
        atomic_store_explicit(&stop, true, memory_order_relaxed);
        pthread_join(producer_th, NULL);
        LamportQueueDestroy(&queue);
        return 1;
    }

    /* ── Run for the requested duration ──────────────────────────────── */
    printf("\nRunning for %d seconds...\n", duration);
    sleep((unsigned int)duration);

    /* Signal stop and join threads */
    atomic_store_explicit(&stop, true, memory_order_relaxed);
    pthread_join(producer_th, NULL);
    pthread_join(consumer_th, NULL);

    /* ── Aggregate stats ─────────────────────────────────────────────── */
    uint64_t total_produced = atomic_load_explicit(&p_args.ops_produced,
                                                   memory_order_relaxed);
    uint64_t total_consumed = atomic_load_explicit(&c_args.ops_consumed,
                                                   memory_order_relaxed);
    uint64_t seq_errors     = atomic_load_explicit(&p_args.sequence_errors,
                                                   memory_order_relaxed)
                            + atomic_load_explicit(&c_args.sequence_errors,
                                                   memory_order_relaxed);
    uint64_t total_ops      = total_produced + total_consumed;
    double   ops_per_sec    = (double)total_ops / (double)duration;
    size_t   remaining      = LamportQueueLeasedSize(&queue);

    /* ── Report ──────────────────────────────────────────────────────── */
    printf("\n=== Results ===\n");
    printf("  Total ops:        %lu\n", (unsigned long)total_ops);
    printf("  Throughput:       %.0f ops/s\n", ops_per_sec);
    printf("  Produced:         %lu\n", (unsigned long)total_produced);
    printf("  Consumed:         %lu\n", (unsigned long)total_consumed);
    printf("  In-flight:        %lu\n", (unsigned long)remaining);
    printf("  Sequence errors:  %lu (must be zero)\n",
           (unsigned long)seq_errors);

    /* ── Consistency check ──────────────────────────────────────────── */
    if (seq_errors > 0) {
        printf("  VERDICT:          FAIL — %lu sequence errors detected\n",
               (unsigned long)seq_errors);
    } else if (total_produced == 0 && total_consumed == 0) {
        printf("  VERDICT:          INCONCLUSIVE — no operations completed\n");
    } else {
        printf("  VERDICT:          PASS\n");
    }
    printf("===================\n");

    /* ── Cleanup ────────────────────────────────────────────────────── */
    LamportQueueDestroy(&queue);

    if (seq_errors > 0) return 1;
    if (total_produced == 0 && total_consumed == 0) return 2;
    return 0;
}
