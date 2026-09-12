/**
 * @file cdt_lamport_queue.h
 * @brief Classic Lamport single-producer, single-consumer (SPSC) lock-free
 *        FIFO queue implemented with C11 atomics.
 *
 * Based on "Specifying and Verifying a FIFO Queue" (Lamport, HAL hal-00862450).
 * One slot is reserved to distinguish full from empty — a queue initialised
 * with @p queue_sz slots has a usable capacity of (@p queue_sz - 1).
 *
 * @b Thread-safety: Exactly one producer and one consumer thread.  Using
 * multiple producers or multiple consumers is undefined behaviour.
 *
 * @code{.c}
 * LocklessSpscQueue queue;
 * memset(&queue, 0, sizeof(queue));
 * LamportQueueInit(&queue, NULL, 64);  // 63 usable slots
 *
 * // Producer
 * LamportQueuePush(&queue, some_data);
 *
 * // Consumer
 * QueueClientData *item = NULL;
 * if (LamportQueuePop(&queue, &item)) { process(item); }
 *
 * LamportQueueDestroy(&queue);
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

#ifndef UFLIB_CDT_CDT_LAMPORT_QUEUE_H
#define UFLIB_CDT_CDT_LAMPORT_QUEUE_H

#include <uflib/uflib_defs.h>
#include <uflib/standard_defs.h>
#include <uflib/cdt/cdt_lamport_queue_type.h>

/**
 * @brief Initialise a lock-free SPSC queue.
 *
 * If @p payload is NULL, an internal payload array of @p queue_sz
 * `QueueClientData *` slots is allocated via calloc() and will be freed
 * by LamportQueueDestroy().  If @p payload is provided, ownership
 * remains with the caller.
 *
 * @p queue_sz values less than 2 are clamped to 2 (minimum viable queue:
 * 1 usable slot).
 *
 * @param queue    Pointer to caller-allocated (or zero-filled) struct.
 * @param payload  Optional pre-allocated array of `QueueClientData *`
 *                 of length @p queue_sz.  May be NULL.
 * @param queue_sz Number of slots.  Usable capacity is `queue_sz - 1`.
 */
PUBLIC_API void LamportQueueInit(LocklessSpscQueue *queue,
                                 QueueClientData **payload,
                                 size_t queue_sz);

/**
 * @brief Destroy a queue, freeing the internal payload array if it was
 *        allocated by LamportQueueInit().
 *
 * Safe to call on a zero-initialised struct (e.g. after memset).
 * Idempotent — calling twice is harmless.  NULL @p queue is safe.
 *
 * @param queue  Queue to destroy.  NULL is safe.
 */
PUBLIC_API void LamportQueueDestroy(LocklessSpscQueue *queue);

/**
 * @brief Push an element onto the queue (producer-only).
 *
 * @param queue  Queue pointer.  NULL returns false (no crash).
 * @param elem   Element to enqueue; may be NULL — the queue stores
 *               opaque pointers without interpretation.
 * @return true  if the element was enqueued,
 * @return false if the queue is full or @p queue is NULL.
 */
PUBLIC_API bool LamportQueuePush(LocklessSpscQueue *queue,
                                 QueueClientData *elem);

/**
 * @brief Pop an element from the queue (consumer-only).
 *
 * @param queue  Queue pointer.  NULL returns false (no crash).
 * @param elem   Output pointer to receive the dequeued element.
 *               NULL returns false (caller must provide valid storage).
 * @return true  if an element was dequeued,
 * @return false if the queue is empty or either argument is NULL.
 */
PUBLIC_API bool LamportQueuePop(LocklessSpscQueue *queue,
                                QueueClientData **elem);

/**
 * @brief Return an approximate leased-element count.
 *
 * Updated atomically by Push (+1) and Pop (-1).  The value is a
 * best-effort snapshot — it is not guaranteed to be precisely
 * synchronised with the head/tail pointers.
 *
 * @param queue  Queue pointer.  NULL returns 0.
 * @return Approximate number of elements currently in the queue.
 */
PUBLIC_API __pure size_t LamportQueueLeasedSize(LocklessSpscQueue *queue);

#endif /* UFLIB_CDT_CDT_LAMPORT_QUEUE_H */
