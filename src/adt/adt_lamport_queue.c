/**
 * Copyright (C) 2015-2021 unfacd works
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

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>
#include <uflib/standard_c_includes.h>
#include <uflib/adt/adt_lamport_queue.h>

/**
 * 	Classic single producer, single consumer FIFO queue a la Lamport https://hal.inria.fr/hal-00862450/document implemented with
 * 	c11 atomics.
 */

void
LamportQueueInit(LocklessSpscQueue *queue, QueueClientData **payload, size_t queue_sz)
{
	atomic_init(&queue->front_, 0);
	atomic_init(&queue->back_, 0);
	atomic_init(&queue->leased, 0);

	queue->cached_front_ = queue->cached_back_ = 0;
	queue->queue_sz = queue_sz;

	if (IS_PRESENT(payload))	queue->payload = payload;
	else											queue->payload = calloc(queue_sz, sizeof(void *));
}

bool
LamportQueuePush(LocklessSpscQueue *queue, QueueClientData *elem)
{
	size_t b, f;

	b = atomic_load_explicit(&queue->back_, memory_order_relaxed);
	f = queue->cached_front_;

	if ((b + 1) % queue->queue_sz == f) {
		queue->cached_front_ = f = atomic_load_explicit(&queue->front_, memory_order_acquire);
	} else {
		/* front can only increase since the last time we read it, which means we can only get more space to push into.
 If we still have space left from the last time we read, we don't have to read again. */
	}

	if ((b + 1) % queue->queue_sz == f) {
    return false;
	} else {
	  /* not full */
	}

	queue->payload[b] = elem;
	atomic_store_explicit(&queue->back_, (b + 1) % queue->queue_sz, memory_order_release);
	atomic_fetch_add_explicit(&(queue->leased), 1, memory_order_release);

	return true;
}

bool
LamportQueuePop(LocklessSpscQueue *queue, QueueClientData **elem)
{
    size_t b, f;
    f = atomic_load_explicit(&queue->front_, memory_order_relaxed);
    b = queue->cached_back_;
    if (b == f) {
	    queue->cached_back_ = b = atomic_load_explicit(&queue->back_, memory_order_acquire);
    } else {
      /* back can only increase since the last time we read it, which means we can only get more items to pop from.
	 	 	 If we still have items left from the last time we read, we don't have to read again. */
		}
    if (b == f) {
      return false;
    } else { /* not empty */ }

    *elem = queue->payload[f];
    atomic_store_explicit(&queue->front_, (f + 1) % queue->queue_sz, memory_order_release);
    atomic_fetch_sub_explicit(&(queue->leased), 1, memory_order_release);

    return true;
}

__pure size_t
LamportQueueLeasedSize (LocklessSpscQueue *queue)
{
	return atomic_load_explicit(&(queue->leased),  memory_order_acquire);
}