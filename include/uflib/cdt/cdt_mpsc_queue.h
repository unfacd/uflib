/**
 * @file cdt_mpsc_queue.h
 * @brief Intrusive lock-free multi-producer, single-consumer (MPSC) queue —
 *        Vyukov algorithm, C11 atomics.
 *
 * Original algorithm: Dmitry Vyukov, "Intrusive MPSC node-based queue"
 * (1024cores.net).  Derived from https://github.com/grivet/mpsc-queue and
 * https://github.com/winksaville/test-mpscfifo-intrusive; commentary at
 * https://u256.net/posts/mpsc-queue.html.
 *
 * Properties:
 * - @b Multi-producer: any number of threads may insert concurrently; an
 *   insertion is thread-safe and costs one atomic exchange (wait-free).
 * - @b Single-consumer: exactly one thread may remove nodes.
 * - @b Unbounded: a linked list — no capacity limit; nodes are
 *   caller-allocated (intrusive).
 * - @b Obstruction-free reads: a poll takes a bounded number of
 *   instructions, but there is no removal forward-guarantee — progress
 *   relies on producers completing their two-step insert.
 * - @b Per-producer FIFO: each producer's items are consumed in that
 *   producer's insertion order.
 * - The queue is serializable but @b not linearizable: an insert consists
 *   of two separate memory transactions, so the queue can transiently
 *   appear inconsistent (MPSC_QUEUE_RETRY) within a series.
 *
 * @b Thread-safety: mpsc_queue_insert() may be called from any thread.
 * mpsc_queue_init(), mpsc_queue_pop(), mpsc_queue_poll(),
 * mpsc_queue_tail(), mpsc_queue_push_front(), and the traversal macros are
 * consumer-side only.
 *
 * @warning To avoid livelocking the consumer, producer threads must not be
 * cancelled or killed while inside mpsc_queue_insert() — use cooperative
 * threads or insert outside cancellable sections.
 *
 * @code{.c}
 * #include <uflib/cdt/cdt_mpsc_queue.h>
 *
 * LocklessMpscQueue queue;
 * mpsc_queue_init(&queue);
 *
 * // Producer (any thread): node storage owned by the caller
 * struct mpsc_queue_node *node = node_pool_take();
 * node->context_data = AS_QUEUE_CONTEXT_DATA(message);
 * mpsc_queue_insert(&queue, node);
 *
 * // Consumer (one thread)
 * struct mpsc_queue_node *popped;
 * while ((popped = mpsc_queue_pop(&queue))) {
 *   process(popped->context_data);
 *   node_pool_return(popped);
 * }
 * @endcode
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

#ifndef UFLIB_CDT_CDT_MPSC_QUEUE_H
#define UFLIB_CDT_CDT_MPSC_QUEUE_H

#include <uflib/uflib_defs.h>
#include <uflib/standard_defs.h>

#include <stdbool.h>

#include <uflib/cdt/cdt_mpsc_queue_type.h>

/* ── Consumer API ──────────────────────────────────────────────────────── */

/**
 * @brief Initialise a queue to the empty state (head = tail = &stub).
 *
 * Must complete before the queue is shared with producers; not safe to
 * call concurrently with any other operation on the same queue.
 *
 * @param queue  Queue to initialise.  NULL is safe (no-op).
 *
 * @code{.c}
 * LocklessMpscQueue queue;
 * mpsc_queue_init(&queue);
 * @endcode
 */
PUBLIC_API void mpsc_queue_init(struct LocklessMpscQueue *queue);

/**
 * @brief Insert a node at the front of the queue (consumer-only).
 *
 * The node becomes the next item mpsc_queue_pop() returns.  Useful for
 * "undo a pop" / requeue-at-front patterns.
 *
 * @param queue  Queue pointer.  NULL is safe (no-op).
 * @param node   Node to insert; must be owned by the consumer (e.g. just
 *               popped).  NULL is safe (no-op).
 *
 * @code{.c}
 * struct mpsc_queue_node *node = mpsc_queue_pop(&queue);
 * if (node && !can_process_yet(node))
 *   mpsc_queue_push_front(&queue, node);  // retry later, order preserved
 * @endcode
 */
PUBLIC_API void mpsc_queue_push_front(struct LocklessMpscQueue *queue, struct mpsc_queue_node *node);

/**
 * @brief One non-blocking dequeue attempt (consumer-only).
 *
 * Unlike mpsc_queue_pop(), this never spins: it reports
 * MPSC_QUEUE_RETRY when a producer has exchanged the head but not yet
 * linked its node (the transient inconsistency window), letting the
 * caller decide how to wait (yield, back off, do other work).
 *
 * @param queue  Queue pointer.  NULL returns MPSC_QUEUE_EMPTY.
 * @param node   Receives the dequeued node on MPSC_QUEUE_ITEM.
 *               NULL returns MPSC_QUEUE_EMPTY.
 * @return MPSC_QUEUE_ITEM and *node set, MPSC_QUEUE_EMPTY if no item,
 *         MPSC_QUEUE_RETRY if an insert is in flight.
 *
 * @code{.c}
 * struct mpsc_queue_node *node;
 * switch (mpsc_queue_poll(&queue, &node)) {
 *   case MPSC_QUEUE_ITEM:  process(node); break;
 *   case MPSC_QUEUE_RETRY: sched_yield(); break;   // producer mid-insert
 *   case MPSC_QUEUE_EMPTY: wait_for_work(); break;
 * }
 * @endcode
 */
PUBLIC_API enum mpsc_queue_poll_result mpsc_queue_poll(struct LocklessMpscQueue *queue, struct mpsc_queue_node **node);

/**
 * @brief Dequeue the oldest reachable node (consumer-only).
 *
 * Convenience wrapper over mpsc_queue_poll() that spins while the poll
 * reports MPSC_QUEUE_RETRY.
 *
 * @warning The internal retry loop is unbounded: if a producer is
 * descheduled (or cancelled) inside mpsc_queue_insert(), this call spins
 * until that producer resumes.  Callers needing bounded behaviour should
 * use mpsc_queue_poll() directly.
 *
 * @param queue  Queue pointer.  NULL returns NULL.
 * @return Dequeued node, or NULL if the queue is empty.
 *
 * @code{.c}
 * struct mpsc_queue_node *node;
 * while ((node = mpsc_queue_pop(&queue))) consume(node);
 * @endcode
 */
PUBLIC_API struct mpsc_queue_node *mpsc_queue_pop(struct LocklessMpscQueue *queue);

/**
 * @brief Peek at the oldest reachable node without removing it
 *        (consumer-only).
 *
 * May advance the internal tail past the stub node as a side effect.
 *
 * @param queue  Queue pointer.  NULL returns NULL.
 * @return Oldest node, or NULL if the queue is empty.
 *
 * @code{.c}
 * struct mpsc_queue_node *oldest = mpsc_queue_tail(&queue);
 * if (oldest) inspect(oldest->context_data);   // still queued
 * @endcode
 */
PUBLIC_API struct mpsc_queue_node *mpsc_queue_tail(struct LocklessMpscQueue *queue);

/**
 * @brief Iterate over queued nodes without dequeuing (consumer-only).
 *
 * Traversal is safe against concurrent producers but sees a snapshot that
 * may stop early at an in-flight insert.
 */
#define MPSC_QUEUE_FOR_EACH(node, queue) \
for (node = mpsc_queue_tail(queue); node != NULL; \
node = atomic_load_explicit(&node->next, memory_order_acquire))

/**
 * @brief Drain the queue, dequeuing every node in turn (consumer-only).
 */
#define MPSC_QUEUE_FOR_EACH_POP(node, queue) \
while ((node = mpsc_queue_pop(queue)))

/* ── Producer API ──────────────────────────────────────────────────────── */

/**
 * @brief Insert a node at the back of the queue (any thread; wait-free).
 *
 * Costs one atomic exchange.  Ownership of @p node passes to the queue;
 * the producer must not touch the node afterwards.
 *
 * @param queue  Queue pointer.  NULL is safe (no-op).
 * @param node   Caller-allocated node with payload fields already set.
 *               NULL is safe (no-op).
 *
 * @warning Do not cancel/kill a thread inside this call — see the
 * file-level livelock note.
 *
 * @code{.c}
 * node->context_data = AS_QUEUE_CONTEXT_DATA(msg);
 * mpsc_queue_insert(&queue, node);
 * @endcode
 */
PUBLIC_API void mpsc_queue_insert(struct LocklessMpscQueue *queue, struct mpsc_queue_node *node);

#endif /* UFLIB_CDT_CDT_MPSC_QUEUE_H */
