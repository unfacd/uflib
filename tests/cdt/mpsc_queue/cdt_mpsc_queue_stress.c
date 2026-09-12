/**
 * @file cdt_mpsc_queue_stress.c
 * @brief Standalone multi-producer stress test for the intrusive lock-free
 *        MPSC queue (LocklessMpscQueue, Vyukov algorithm).
 *
 * Usage:
 *   cdt_mpsc_queue_stress [--threads N] [--duration S] [--nodes M]
 *
 * Defaults: threads=8 (producers), duration=5, nodes=4096 (per producer).
 *
 * N producer threads insert nodes drawn from private per-producer pools;
 * one consumer thread pops and validates.  The queue is intrusive, so the
 * full ownership hand-off is exercised: a producer may only reuse a node
 * after the consumer has marked it consumed.
 *
 * Zero-error assertions:
 *   - per-producer sequence numbers arrive strictly monotonically (+1)
 *     (per-producer FIFO guarantee);
 *   - no node is delivered twice (consumed-flag check);
 *   - after shutdown, consumed == produced exactly and the queue drains
 *     to empty.
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

#include <uflib/cdt/cdt_mpsc_queue.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>

/* ── Defaults ─────────────────────────────────────────────────────────── */

#define DEFAULT_THREADS  8
#define DEFAULT_DURATION 5
#define DEFAULT_NODES    4096

#define MAX_PRODUCERS    256

/*
 * Payload encoding: | producer:8 | node_idx:16 | seq:40 |
 * seq wraps far beyond any realistic run length.
 */
#define ENCODE(pid, idx, seq) \
	((uintptr_t)(((uint64_t)(pid) << 56) | ((uint64_t)(idx) << 40) | ((seq) & ((1ULL << 40) - 1))))
#define DECODE_PID(v) ((uint32_t)((uint64_t)(v) >> 56))
#define DECODE_IDX(v) ((uint32_t)(((uint64_t)(v) >> 40) & 0xFFFFU))
#define DECODE_SEQ(v) ((uint64_t)(v) & ((1ULL << 40) - 1))

/* ── Per-producer state ───────────────────────────────────────────────── */

typedef struct {
	uint32_t                 producer_id;
	uint32_t                 nodes;         /* pool size */
	struct mpsc_queue_node  *pool;          /* private node pool */
	_Atomic uint8_t         *consumed;      /* per-node hand-back flags */
	struct LocklessMpscQueue *queue;
	_Atomic bool            *stop;
	_Atomic uint32_t        *live;          /* count of producers still running */
	_Atomic uint64_t         produced;
	_Atomic uint64_t         reuse_spins;   /* diagnostic only */
} ProducerArgs;

typedef struct {
	struct LocklessMpscQueue *queue;
	ProducerArgs             *producers;
	uint32_t                  n_producers;
	_Atomic uint32_t         *live;          /* producers still running */
	_Atomic uint64_t          consumed;
	_Atomic uint64_t          seq_errors;    /* per-producer FIFO violations */
	_Atomic uint64_t          dup_errors;    /* double delivery */
	uint64_t                  expected_seq[MAX_PRODUCERS];
} ConsumerArgs;

/* ── Workers ──────────────────────────────────────────────────────────── */

static void *
sProducer(void *arg)
{
	ProducerArgs *a = (ProducerArgs *)arg;
	uint64_t      seq = 0;
	uint32_t      idx = 0;

	while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
		struct mpsc_queue_node *node = &a->pool[idx];

		/*
		 * Intrusive ownership contract: the node may only be reused
		 * after the consumer handed it back (consumed flag set).
		 * Acquire pairs with the consumer's release store.
		 */
		if (!atomic_load_explicit(&a->consumed[idx], memory_order_acquire)) {
			atomic_fetch_add_explicit(&a->reuse_spins, 1,
			                          memory_order_relaxed);
			sched_yield();
			continue;
		}
		atomic_store_explicit(&a->consumed[idx], 0, memory_order_relaxed);

		node->context_data =
		    AS_QUEUE_CONTEXT_DATA(ENCODE(a->producer_id, idx, seq));
		mpsc_queue_insert(a->queue, node);

		atomic_fetch_add_explicit(&a->produced, 1, memory_order_relaxed);
		seq++;
		idx = (idx + 1) % a->nodes;
	}

	/*
	 * Release: every insert and counter update above happens-before the
	 * consumer's acquire observation of the decremented live count —
	 * once live == 0, all produced totals are final and all inserted
	 * nodes are fully linked.
	 */
	atomic_fetch_sub_explicit(a->live, 1, memory_order_release);

	return NULL;
}

static void *
sConsumer(void *arg)
{
	ConsumerArgs *a = (ConsumerArgs *)arg;

	for (;;) {
		struct mpsc_queue_node *node = mpsc_queue_pop(a->queue);

		if (node == NULL) {
			/*
			 * Empty: exit only when every producer has fully exited
			 * (live == 0 → all inserts linked, all counters final)
			 * AND everything produced has been consumed.  Checking a
			 * mere stop *flag* here would race the final in-flight
			 * two-step insert.
			 */
			if (atomic_load_explicit(a->live, memory_order_acquire) == 0) {
				uint64_t produced_total = 0;
				for (uint32_t p = 0; p < a->n_producers; p++)
					produced_total += atomic_load_explicit(
					    &a->producers[p].produced, memory_order_relaxed);
				if (atomic_load_explicit(&a->consumed,
				                         memory_order_relaxed) >= produced_total)
					break;
			}
			sched_yield();
			continue;
		}

		uintptr_t v   = (uintptr_t)node->context_data;
		uint32_t  pid = DECODE_PID(v);
		uint32_t  idx = DECODE_IDX(v);
		uint64_t  seq = DECODE_SEQ(v);

		ProducerArgs *prod = &a->producers[pid];

		/* Per-producer FIFO: sequence must be exactly the expected one. */
		if (seq != a->expected_seq[pid]) {
			uint64_t prev = atomic_fetch_add_explicit(
			    &a->seq_errors, 1, memory_order_relaxed);
			if (prev == 0) {
				fprintf(stderr,
				        "SEQUENCE ERROR: producer %u expected %llu, got %llu\n",
				        pid,
				        (unsigned long long)a->expected_seq[pid],
				        (unsigned long long)seq);
			}
		}
		a->expected_seq[pid] = seq + 1;

		/* Duplicate delivery: the flag must be clear while in flight. */
		if (atomic_load_explicit(&prod->consumed[idx], memory_order_relaxed)) {
			uint64_t prev = atomic_fetch_add_explicit(
			    &a->dup_errors, 1, memory_order_relaxed);
			if (prev == 0) {
				fprintf(stderr,
				        "DUPLICATE ERROR: producer %u node %u seq %llu\n",
				        pid, idx, (unsigned long long)seq);
			}
		}

		/*
		 * Hand the node back to its producer.  Release pairs with the
		 * producer's acquire load — the producer must not observe the
		 * flag before our reads of the node's payload completed.
		 */
		atomic_store_explicit(&prod->consumed[idx], 1, memory_order_release);

		atomic_fetch_add_explicit(&a->consumed, 1, memory_order_relaxed);
	}

	return NULL;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
	uint32_t n_producers = DEFAULT_THREADS;
	uint32_t duration    = DEFAULT_DURATION;
	uint32_t nodes       = DEFAULT_NODES;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
			n_producers = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
			duration = (uint32_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "--nodes") == 0 && i + 1 < argc) {
			nodes = (uint32_t)atoi(argv[++i]);
		} else {
			fprintf(stderr,
			        "Usage: %s [--threads N] [--duration S] [--nodes M]\n",
			        argv[0]);
			return 1;
		}
	}

	if (n_producers == 0 || n_producers > MAX_PRODUCERS || nodes == 0 ||
	    nodes > 0xFFFFU || duration == 0) {
		fprintf(stderr,
		        "invalid args: threads 1..%d, nodes 1..65535, duration >= 1\n",
		        MAX_PRODUCERS);
		return 1;
	}

	printf("=== LocklessMpscQueue Stress Test ===\n");
	printf("  Producers:  %u\n", n_producers);
	printf("  Duration:   %u s\n", duration);
	printf("  Node pool:  %u per producer\n", nodes);
	printf("=====================================\n\n");

	struct LocklessMpscQueue queue;
	mpsc_queue_init(&queue);

	_Atomic bool     stop = false;
	_Atomic uint32_t live = n_producers;

	ProducerArgs *producers = calloc(n_producers, sizeof(*producers));
	if (!producers) { perror("calloc"); return 1; }

	for (uint32_t p = 0; p < n_producers; p++) {
		producers[p].producer_id = p;
		producers[p].nodes       = nodes;
		producers[p].queue       = &queue;
		producers[p].stop        = &stop;
		producers[p].live        = &live;
		producers[p].pool        = calloc(nodes, sizeof(struct mpsc_queue_node));
		producers[p].consumed    = calloc(nodes, sizeof(_Atomic uint8_t));
		if (!producers[p].pool || !producers[p].consumed) {
			perror("calloc");
			return 1;
		}
		/* All nodes start as "handed back" — available for first use. */
		for (uint32_t i = 0; i < nodes; i++)
			atomic_store_explicit(&producers[p].consumed[i], 1,
			                      memory_order_relaxed);
	}

	ConsumerArgs consumer = { 0 };
	consumer.queue       = &queue;
	consumer.producers   = producers;
	consumer.n_producers = n_producers;
	consumer.live        = &live;

	struct timespec t_start, t_end;
	clock_gettime(CLOCK_MONOTONIC, &t_start);

	pthread_t consumer_tid;
	pthread_t *producer_tids = calloc(n_producers, sizeof(pthread_t));
	if (!producer_tids) { perror("calloc"); return 1; }

	pthread_create(&consumer_tid, NULL, sConsumer, &consumer);
	for (uint32_t p = 0; p < n_producers; p++)
		pthread_create(&producer_tids[p], NULL, sProducer, &producers[p]);

	printf("Running for %u seconds...\n\n", duration);
	sleep(duration);

	atomic_store_explicit(&stop, true, memory_order_release);

	for (uint32_t p = 0; p < n_producers; p++)
		pthread_join(producer_tids[p], NULL);
	pthread_join(consumer_tid, NULL);

	clock_gettime(CLOCK_MONOTONIC, &t_end);
	double elapsed = (double)(t_end.tv_sec - t_start.tv_sec) +
	                 (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

	uint64_t produced_total = 0, reuse_spins = 0;
	for (uint32_t p = 0; p < n_producers; p++) {
		produced_total += atomic_load_explicit(&producers[p].produced,
		                                       memory_order_relaxed);
		reuse_spins    += atomic_load_explicit(&producers[p].reuse_spins,
		                                       memory_order_relaxed);
	}
	uint64_t consumed_total = atomic_load_explicit(&consumer.consumed,
	                                               memory_order_relaxed);
	uint64_t seq_errors     = atomic_load_explicit(&consumer.seq_errors,
	                                               memory_order_relaxed);
	uint64_t dup_errors     = atomic_load_explicit(&consumer.dup_errors,
	                                               memory_order_relaxed);

	/* Post-shutdown invariant: the queue must be fully drained. */
	uint64_t residue = 0;
	while (mpsc_queue_pop(&queue) != NULL) residue++;

	uint64_t total_ops = produced_total + consumed_total;

	printf("=== Results ===\n");
	printf("  Total ops:        %llu\n", (unsigned long long)total_ops);
	printf("  Throughput:       %.0f ops/s\n", (double)total_ops / elapsed);
	printf("  Inserts:          %llu\n", (unsigned long long)produced_total);
	printf("  Pops:             %llu\n", (unsigned long long)consumed_total);
	printf("  Reuse spins:      %llu (diagnostic)\n",
	       (unsigned long long)reuse_spins);
	printf("  Sequence errors:  %llu (must be zero)\n",
	       (unsigned long long)seq_errors);
	printf("  Duplicate errors: %llu (must be zero)\n",
	       (unsigned long long)dup_errors);
	printf("  Drain mismatch:   %llu produced vs %llu consumed"
	       " (+%llu residue — must match, residue 0)\n",
	       (unsigned long long)produced_total,
	       (unsigned long long)consumed_total,
	       (unsigned long long)residue);
	printf("===================\n");

	int failed = (seq_errors != 0) || (dup_errors != 0) ||
	             (produced_total != consumed_total) || (residue != 0);

	for (uint32_t p = 0; p < n_producers; p++) {
		free(producers[p].pool);
		free((void *)producers[p].consumed);
	}
	free(producers);
	free(producer_tids);

	printf("Stress test %s.\n", failed ? "FAILED" : "complete");
	return failed ? 1 : 0;
}
