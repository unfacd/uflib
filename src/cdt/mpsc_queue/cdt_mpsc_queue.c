/**
 * @file cdt_mpsc_queue.c
 * @brief Intrusive lock-free multi-producer, single-consumer (MPSC) queue —
 *        Vyukov algorithm, C11 atomics implementation.
 *
 * Original algorithm: Dmitry Vyukov, "Intrusive MPSC node-based queue"
 * (https://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue).
 * Derived from https://github.com/grivet/mpsc-queue/blob/main/mpsc-queue.h
 * and https://github.com/winksaville/test-mpscfifo-intrusive/blob/master/mpscfifo.c;
 * commentary at https://u256.net/posts/mpsc-queue.html.
 *
 * Algorithm: producers publish in two steps — (1) atomically exchange
 * `head` to the new node, (2) link the previous head's `next` to the new
 * node.  The window between the two steps is the queue's transient
 * inconsistency: the consumer can reach the old chain end but not the new
 * head.  The consumer detects this (`tail != head` with `next == NULL`)
 * and reports MPSC_QUEUE_RETRY rather than blocking.  An embedded stub
 * node decouples the two ends; the consumer re-inserts the stub whenever
 * it removes the last real element.
 *
 * The queue is serializable but not linearizable — see the header for the
 * concurrency-environment implications (producers must not be cancelled
 * mid-insert, or the consumer can livelock).
 *
 * Memory ordering:
 *   - Producer XCHG on `head` is `acq_rel`: the release half publishes the
 *     node's `next = NULL` initialisation to the successor producer (RMW
 *     chain), the acquire half ensures this producer sees its
 *     predecessor's initialisation of the node it is about to link
 *     through.
 *   - The producer's store to `prev->next` is `release`: it pairs with the
 *     consumer's `acquire` load of `next`, guaranteeing all payload writes
 *     made before mpsc_queue_insert() are visible to the consumer that
 *     observes the link.
 *   - The consumer's load of `head` is `acquire`: it pairs with the XCHG
 *     release so the `tail != head` mid-insert test is not vacuous on
 *     weakly-ordered architectures.
 *   - `tail` is consumer-private (single consumer): all its loads/stores
 *     are `relaxed` — atomicity without barriers.
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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>
#include <uflib/cdt/cdt_mpsc_queue.h>

/* ── Consumer API ──────────────────────────────────────────────────────── */

void
mpsc_queue_init(struct LocklessMpscQueue *queue)
{
	if (unlikely(!queue)) return;

	/*
	 * Relaxed stores: initialisation happens-before any sharing of the
	 * queue with other threads (caller's responsibility) — no
	 * publication ordering is needed here.
	 */
	atomic_store_explicit(&queue->head, &queue->stub, memory_order_relaxed);
	atomic_store_explicit(&queue->tail, &queue->stub, memory_order_relaxed);
	atomic_store_explicit(&queue->stub.next, NULL, memory_order_relaxed);
}

void
mpsc_queue_push_front(struct LocklessMpscQueue *queue, struct mpsc_queue_node *node)
{
	struct mpsc_queue_node *tail;

	if (unlikely(!queue || !node)) return;

	/*
	 * Consumer-only operation: `tail` and the front node are owned by
	 * the single consumer thread, and producers never read `tail` —
	 * relaxed suffices for all three accesses.
	 */
	tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
	atomic_store_explicit(&node->next, tail, memory_order_relaxed);
	atomic_store_explicit(&queue->tail, node, memory_order_relaxed);
}

enum mpsc_queue_poll_result
mpsc_queue_poll(struct LocklessMpscQueue *queue, struct mpsc_queue_node **node)
{
	struct mpsc_queue_node *tail;
	struct mpsc_queue_node *next;
	struct mpsc_queue_node *head;

	if (unlikely(!queue || !node)) return MPSC_QUEUE_EMPTY;

	tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
	/*
	 * Acquire pairs with the producer's release store to `prev->next` in
	 * mpsc_queue_insert() — observing the link guarantees visibility of
	 * every payload write the producer made before inserting.
	 */
	next = atomic_load_explicit(&tail->next, memory_order_acquire);

	if (tail == &queue->stub) {
		if (next == NULL) {
			return MPSC_QUEUE_EMPTY;
		}

		// Advance tail past the stub to the real "tail"
		atomic_store_explicit(&queue->tail, next, memory_order_relaxed);
		tail = next;
		next = atomic_load_explicit(&tail->next, memory_order_acquire);
	}

	if (next != NULL) {
		atomic_store_explicit(&queue->tail, next, memory_order_relaxed);
		*node = tail;
		return MPSC_QUEUE_ITEM;
	}

	/*
	 * next == NULL: either this is the last element, or a producer was
	 * preempted between its XCHG and its next-link store.  Distinguish
	 * by testing tail != head — acquire pairs with the XCHG release.
	 */
	head = atomic_load_explicit(&queue->head, memory_order_acquire);
	if (tail != head) {
		return MPSC_QUEUE_RETRY;
	}

	/*
	 * (tail == head): last element.  Removing it would leave the queue
	 * with no node at all, so re-insert the stub first.
	 */
	mpsc_queue_insert(queue, &queue->stub);

	next = atomic_load_explicit(&tail->next, memory_order_acquire);
	if (next != NULL) {
		atomic_store_explicit(&queue->tail, next, memory_order_relaxed);
		*node = tail;
		return MPSC_QUEUE_ITEM;
	}

	/*
	 * A producer XCHG'd head between our head load and our stub insert
	 * and has not linked yet — indistinguishable from empty this round.
	 */
	return MPSC_QUEUE_EMPTY;
}

struct mpsc_queue_node *
mpsc_queue_pop(struct LocklessMpscQueue *queue)
{
	enum mpsc_queue_poll_result result;
	struct mpsc_queue_node *node;

	if (unlikely(!queue)) return NULL;

	do {
		result = mpsc_queue_poll(queue, &node);
		if (result == MPSC_QUEUE_EMPTY) {
			return NULL;
		}
	} while (result == MPSC_QUEUE_RETRY);

	return node;
}

struct mpsc_queue_node *
mpsc_queue_tail(struct LocklessMpscQueue *queue)
{
	struct mpsc_queue_node *tail;
	struct mpsc_queue_node *next;

	if (unlikely(!queue)) return NULL;

	tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
	/* Acquire: see mpsc_queue_poll() — payload visibility via the link. */
	next = atomic_load_explicit(&tail->next, memory_order_acquire);

	if (tail == &queue->stub) {
		if (next == NULL) {
			return NULL;
		}

		atomic_store_explicit(&queue->tail, next, memory_order_relaxed);
		tail = next;
	}

	return tail;
}

/* ── Producer API ──────────────────────────────────────────────────────── */

void
mpsc_queue_insert(struct LocklessMpscQueue *queue, struct mpsc_queue_node *node)
{
	struct mpsc_queue_node *prev;

	if (unlikely(!queue || !node)) return;

	/*
	 * Relaxed: the node is not yet reachable by any other thread; the
	 * XCHG release below orders this initialisation before publication.
	 */
	atomic_store_explicit(&node->next, NULL, memory_order_relaxed);

	/*
	 * Wait-free publication point.  acq_rel: release publishes the
	 * `next = NULL` init to the successor producer through the RMW
	 * chain; acquire orders this thread against its predecessor before
	 * the link store below writes into the predecessor's node.
	 */
	prev = atomic_exchange_explicit(&queue->head, node, memory_order_acq_rel);

	/*
	 * Release pairs with the consumer's acquire load of `next`: every
	 * payload write made before this call is visible to the consumer
	 * that observes the link.  Between the XCHG above and this store
	 * the queue is transiently unreachable past `prev` — the consumer
	 * reports MPSC_QUEUE_RETRY for that window.
	 */
	atomic_store_explicit(&prev->next, node, memory_order_release);
}
