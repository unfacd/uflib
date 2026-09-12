/**
 * @file cdt_lockless_ringbuffer.h
 * @brief Lock-free bounded ring buffer — public API (SPSC / MPSC / MPMC).
 *
 * A bounded, lock-free, copy-in/copy-out ring buffer for arbitrary POD
 * payloads.  Elements are stored by value (`elem_size` bytes each); the
 * buffer owns no payload resources and never calls constructors,
 * destructors, or finalisers.
 *
 * Three concurrency modes are supported (see LocklessRingBufferMode):
 *
 *   - SPSC — one producer, one consumer.  Two-index ring buffer with a
 *     single reserved slot; storable capacity is `capacity - 1`.
 *   - MPSC — N producers, one consumer.  Vyukov per-slot sequence numbers;
 *     full `capacity` usable.
 *   - MPMC — N producers, N consumers.  Vyukov per-slot sequence numbers;
 *     full `capacity` usable.
 *
 * All operations are lock-free and non-blocking: a full push or an empty
 * pop returns false rather than blocking.  Progress is guaranteed while at
 * least one thread per role is live; a thread suspended inside a CAS loop
 * does not block the others.
 *
 * @warning The capacity reported by LocklessRingBufferCapacity() is the
 * storable element count and differs by mode: SPSC reserves one slot as its
 * full/empty sentinel, so it reports `power_of_two - 1`; MPSC and MPMC
 * report the full `power_of_two`.
 *
 * @warning Elements are trivially-copyable by value.  Passing a type with a
 * non-trivial constructor/destructor is a bug (no lifetime management is
 * performed on the stored bytes).
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

#ifndef UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_H
#define UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_H

#include <stdbool.h>
#include <stddef.h>

#include <uflib/uflib_defs.h>
#include <uflib/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer_type.h>

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Create a new lock-free bounded ring buffer.
 *
 * All memory is allocated upfront — there are zero dynamic allocations
 * during the buffer's operational lifetime.
 *
 * @param[in] capacity   Requested maximum element count; rounded up to the
 *                       next power of two internally.  Must be non-zero.
 * @param[in] elem_size  Size of each element in bytes.  Must be non-zero.
 * @param[in] mode       Concurrency mode (SPSC / MPSC / MPMC).
 *
 * @return Opaque handle, or NULL on failure (zero capacity/elem_size,
 *         invalid mode, size overflow, or allocation failure).
 *
 * @code{.c}
 * LocklessRingBuffer *ring_ptr =
 *     LocklessRingBufferCreate(1024, sizeof(int), LOCKLESS_RINGBUF_MODE_MPSC);
 * if (!ring_ptr) { // handle failure }
 * LocklessRingBufferDestroy(ring_ptr);
 * @endcode
 */
PUBLIC_API LocklessRingBuffer *
LocklessRingBufferCreate(size_t capacity, size_t elem_size, LocklessRingBufferMode mode);

/**
 * @brief Tear down the buffer and free all internal storage.
 *
 * Must only be called once no other thread is accessing the buffer.  NULL
 * is safe (no-op).  Does not free any payloads — those are stored by value
 * and caller-owned.
 *
 * @param[in,out] ring_ptr  Buffer handle (NULL → no-op).
 */
PUBLIC_API void
LocklessRingBufferDestroy(LocklessRingBuffer *ring_ptr);

/* ── Core operations ───────────────────────────────────────────────────── */

/**
 * @brief Try to push one element (by-value copy).
 *
 * Non-blocking and lock-free.  Returns false if the buffer is full; the
 * element is not consumed in that case.
 *
 * @param[in,out] ring_ptr  Buffer handle (NULL → false).
 * @param[in]     elem_ptr  Pointer to the element to copy in (NULL → false).
 *
 * @return true if the element was enqueued, false if full or invalid args.
 *
 * @code{.c}
 * int value = 42;
 * if (!LocklessRingBufferTryPush(ring_ptr, &value)) {
 *     // full — apply backpressure or drop
 * }
 * @endcode
 */
PUBLIC_API bool
LocklessRingBufferTryPush(LocklessRingBuffer *ring_ptr, const void *elem_ptr);

/**
 * @brief Try to pop one element (by-value copy out).
 *
 * Non-blocking and lock-free.  Returns false if the buffer is empty; the
 * output buffer is left untouched in that case.
 *
 * @param[in,out] ring_ptr  Buffer handle (NULL → false).
 * @param[out]    elem_ptr  Buffer to receive the element (NULL → false).
 *
 * @return true if an element was dequeued, false if empty or invalid args.
 *
 * @code{.c}
 * int value;
 * if (LocklessRingBufferTryPop(ring_ptr, &value)) {
 *     // process value
 * }
 * @endcode
 */
PUBLIC_API bool
LocklessRingBufferTryPop(LocklessRingBuffer *ring_ptr, void *elem_ptr);

/* ── Introspection ─────────────────────────────────────────────────────── */

/**
 * @brief Current element count (approximate under contention).
 *
 * @param[in] ring_ptr  Buffer handle (NULL → 0).
 *
 * @return Number of elements currently stored.  Under concurrent mutation
 *         this is a snapshot and may be stale by the time the caller reads
 *         it; it is exact only when the caller guarantees quiescence.
 */
PUBLIC_API size_t
LocklessRingBufferSize(const LocklessRingBuffer *ring_ptr);

/**
 * @brief Maximum number of elements the buffer can hold (storable count).
 *
 * @param[in] ring_ptr  Buffer handle (NULL → 0).
 *
 * @return Storable capacity.  SPSC reports `power_of_two - 1` (one reserved
 *         slot); MPSC/MPMC report the full `power_of_two`.
 */
PUBLIC_API size_t
LocklessRingBufferCapacity(const LocklessRingBuffer *ring_ptr);

/**
 * @brief Whether the buffer currently holds zero elements.
 *
 * Approximate under contention (same snapshot semantics as
 * LocklessRingBufferSize()).
 *
 * @param[in] ring_ptr  Buffer handle (NULL → true).
 *
 * @return true if empty.
 */
PUBLIC_API bool
LocklessRingBufferEmpty(const LocklessRingBuffer *ring_ptr);

/**
 * @brief Whether the buffer is at its storable capacity.
 *
 * Approximate under contention.  When true, the next
 * LocklessRingBufferTryPush() will return false (unless a concurrent
 * consumer frees a slot first).
 *
 * @param[in] ring_ptr  Buffer handle (NULL → false).
 *
 * @return true if full.
 */
PUBLIC_API bool
LocklessRingBufferFull(const LocklessRingBuffer *ring_ptr);

/* ── Typed convenience macro ───────────────────────────────────────────── */

/**
 * @brief Generate a small typed wrapper around the void* API.
 *
 * Expands to a typedef (`NAME_t`) and a set of `static inline` helper
 * functions.  The wrapper adds type-safety at the call site: the element
 * type is fixed, so `NAME_t` need not be cast to/from `void *`.
 *
 * `NAME` is the PascalCase module prefix; `TYPE` must be a complete,
 * trivially-copyable type; `CAPACITY` is the element count (same rounding
 * and mode rules as LocklessRingBufferCreate()); `MODE` is a
 * LocklessRingBufferMode enumerator.
 *
 * @code{.c}
 * CDT_RINGBUF_DEFINE(IntQueue, int, 1024, LOCKLESS_RINGBUF_MODE_MPSC)
 *
 * IntQueue_t *queue_ptr = IntQueueCreate();
 * IntQueueTryPush(queue_ptr, 42);
 * int value;
 * if (IntQueueTryPop(queue_ptr, &value)) { // process value }
 * IntQueueDestroy(queue_ptr);
 * @endcode
 */
#define CDT_RINGBUF_DEFINE(NAME, TYPE, CAPACITY, MODE)                        \
    typedef LocklessRingBuffer NAME##_t;                                      \
    static inline NAME##_t *                                                  \
    NAME##Create(void)                                                        \
    {                                                                         \
        return LocklessRingBufferCreate((CAPACITY), sizeof(TYPE), (MODE));    \
    }                                                                         \
    static inline void                                                        \
    NAME##Destroy(NAME##_t *queue_ptr)                                        \
    {                                                                         \
        LocklessRingBufferDestroy(queue_ptr);                                 \
    }                                                                         \
    static inline bool                                                        \
    NAME##TryPush(NAME##_t *queue_ptr, TYPE value)                            \
    {                                                                         \
        return LocklessRingBufferTryPush(queue_ptr, &value);                  \
    }                                                                         \
    static inline bool                                                        \
    NAME##TryPop(NAME##_t *queue_ptr, TYPE *value_ptr)                        \
    {                                                                         \
        return LocklessRingBufferTryPop(queue_ptr, value_ptr);                \
    }                                                                         \
    static inline size_t                                                      \
    NAME##Size(const NAME##_t *queue_ptr)                                     \
    {                                                                         \
        return LocklessRingBufferSize(queue_ptr);                             \
    }                                                                         \
    static inline bool                                                        \
    NAME##Empty(const NAME##_t *queue_ptr)                                    \
    {                                                                         \
        return LocklessRingBufferEmpty(queue_ptr);                            \
    }                                                                         \
    static inline bool                                                        \
    NAME##Full(const NAME##_t *queue_ptr)                                     \
    {                                                                         \
        return LocklessRingBufferFull(queue_ptr);                             \
    }

#endif /* UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_H */
