/**
 * @file cdt_lamport_queue.c
 * @brief Classic Lamport single-producer, single-consumer (SPSC) lock-free
 *        FIFO queue — C11 atomics implementation.
 *
 * Based on "Specifying and Verifying a FIFO Queue" (Lamport, HAL hal-00862450).
 *
 * Algorithm: The producer writes to the buffer at index `back_` and increments
 * `back_`; the consumer reads from `front_` and increments `front_`.  Each side
 * caches the other's index locally to avoid expensive atomic loads — it only
 * refreshes from the shared atomic when the cache indicates the queue may be
 * full (producer) or empty (consumer).
 *
 * Memory ordering:
 *   - `back_`  is written by the producer with `release` and read by the
 *     consumer with `acquire`.  This ensures the payload write is visible
 *     before the consumer observes the slot as occupied.
 *   - `front_` is written by the consumer with `release` and read by the
 *     producer with `acquire`.  This ensures the consumer has finished
 *     reading before the producer reclaims the slot.
 *   - `leased` is a standalone diagnostic counter — it carries no associated
 *     payload and uses `relaxed` ordering (atomicity without barriers).
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
#include <uflib/cdt/cdt_lamport_queue.h>

void
LamportQueueInit(LocklessSpscQueue *queue, QueueClientData **payload,
                 size_t queue_sz)
{
	if (unlikely(!queue)) return;

	/* Clamp to minimum viable queue: 2 slots → 1 usable. */
	if (queue_sz < 2) queue_sz = 2;

	atomic_init(&queue->front_, 0);
	atomic_init(&queue->back_, 0);
	atomic_init(&queue->leased, 0);

	queue->cached_front_ = queue->cached_back_ = 0;
	queue->queue_sz = queue_sz;
	queue->owns_payload = false;

	if (IS_PRESENT(payload)) {
		queue->payload = payload;
	} else {
		queue->payload = calloc(queue_sz, sizeof(void *));
		if (likely(queue->payload)) {
			queue->owns_payload = true;
		}
	}
}

void
LamportQueueDestroy(LocklessSpscQueue *queue)
{
	if (unlikely(!queue)) return;

	if (queue->owns_payload && IS_PRESENT(queue->payload)) {
		free(queue->payload);
		queue->payload = NULL;
		queue->owns_payload = false;
	}

	/* Zero the struct so re-init or double-destroy is safe. */
	memset(queue, 0, sizeof(*queue));
}

bool
LamportQueuePush(LocklessSpscQueue *queue, QueueClientData *elem)
{
	size_t b, f;

	if (unlikely(!queue)) return false;

	b = atomic_load_explicit(&queue->back_, memory_order_relaxed);
	f = queue->cached_front_;

	if ((b + 1) % queue->queue_sz == f) {
		/*
		 * The cached front_ suggests the queue is full — refresh from
		 * the consumer's shared atomic.  front_ can only have increased
		 * since we last read it, so this can only discover more space.
		 */
		queue->cached_front_ = f = atomic_load_explicit(
		    &queue->front_, memory_order_acquire);
	}

	if ((b + 1) % queue->queue_sz == f) {
		return false;  /* genuinely full */
	}

	queue->payload[b] = elem;

	/*
	 * Release-store pairs with the consumer's acquire-load of back_,
	 * guaranteeing the payload write above is visible before the
	 * consumer observes the updated back_ index.
	 */
	atomic_store_explicit(&queue->back_, (b + 1) % queue->queue_sz,
	                      memory_order_release);

	/*
	 * The leased counter is a standalone diagnostic metric — it carries
	 * no associated payload and no thread synchronises on its value.
	 * The actual data publication happens through the release/acquire
	 * pair on back_ (above).  Using memory_order_release here would
	 * force an unnecessary store-buffer drain on weakly-ordered
	 * architectures (ARM, PowerPC) for every push — relaxed is
	 * sufficient for a counter whose only contract is atomicity
	 * (no torn writes).
	 */
	atomic_fetch_add_explicit(&queue->leased, 1, memory_order_relaxed);

	return true;
}

bool
LamportQueuePop(LocklessSpscQueue *queue, QueueClientData **elem)
{
	size_t b, f;

	if (unlikely(!queue || !elem)) return false;

	f = atomic_load_explicit(&queue->front_, memory_order_relaxed);
	b = queue->cached_back_;

	if (b == f) {
		/*
		 * The cached back_ suggests the queue is empty — refresh from
		 * the producer's shared atomic.  back_ can only have increased
		 * since we last read it, so this can only discover more items.
		 */
		queue->cached_back_ = b = atomic_load_explicit(
		    &queue->back_, memory_order_acquire);
	}

	if (b == f) {
		return false;  /* genuinely empty */
	}

	*elem = queue->payload[f];

	/*
	 * Release-store pairs with the producer's acquire-load of front_,
	 * guaranteeing the payload read above completes before the
	 * producer observes the slot as reclaimed.
	 */
	atomic_store_explicit(&queue->front_, (f + 1) % queue->queue_sz,
	                      memory_order_release);

	/* See comment in LamportQueuePush — leased is standalone, relaxed suffices. */
	atomic_fetch_sub_explicit(&queue->leased, 1, memory_order_relaxed);

	return true;
}

__pure size_t
LamportQueueLeasedSize(LocklessSpscQueue *queue)
{
	if (unlikely(!queue)) return 0;

	/*
	 * Relaxed load is sufficient: the counter's modification order is
	 * coherent (no torn values), and the caller receives a best-effort
	 * snapshot — there is no data guarded by this load that requires
	 * acquire ordering.
	 */
	return atomic_load_explicit(&queue->leased, memory_order_relaxed);
}
