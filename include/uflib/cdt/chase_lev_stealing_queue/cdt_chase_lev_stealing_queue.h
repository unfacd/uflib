/**
 * @file cdt_chase_lev_stealing_queue.h
 * @brief Chase–Lev work-stealing deque — public API.
 *
 * A single-owner, multi-thief Chase–Lev deque (Chase & Lev 2005; portable
 * C11/weak-memory treatment by Lê, Pop, Cohen, Zappa Nardelli 2013), with a
 * conservative retain-until-destroy buffer-lifetime policy.
 *
 * Roles:
 *
 *   - @b Owner (exactly one thread): `Push` (LIFO at the bottom end) and
 *     `Pop` (LIFO from the bottom end).
 *   - @b Thieves (any number of threads): `Steal` (FIFO from the top end).
 *
 * The ring grows dynamically by doubling; only the owner grows, and old
 * rings are retained until `Destroy` (geometric growth bounds the retained
 * memory to less than the current capacity).  All hot paths are lock-free;
 * allocation happens only on growth.
 *
 * @warning `NULL` is not a legal item.  `Push` with a `NULL` item fails
 * closed (returns false), because `NULL` would be ambiguous with an empty
 * slot.
 *
 * @warning `Destroy` requires external quiescence: no `Push`/`Pop`/`Steal`
 * may be in flight.  The owner must also be the thread that calls `Destroy`
 * in practice (or a thread that has drained/joined the thieves).
 *
 * @code{.c}
 * #include <uflib/cdt/chase_lev_stealing_queue/cdt_chase_lev_stealing_queue.h>
 *
 * ChaseLevStealingQueue *q = ChaseLevStealingQueueCreate(64);
 *
 * // Owner (one thread):
 * ChaseLevStealingQueuePush(q, task);
 * void *task;
 * if (ChaseLevStealingQueuePop(q, &task)) run(task);   // LIFO
 *
 * // Thief (any other thread):
 * if (ChaseLevStealingQueueSteal(q, &task)) run(task); // FIFO
 *
 * ChaseLevStealingQueueDestroy(q);                    // after quiescence
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

#ifndef UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_CDT_CHASE_LEV_STEALING_QUEUE_H
#define UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_CDT_CHASE_LEV_STEALING_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>
#include <uflib/cdt/chase_lev_stealing_queue/cdt_chase_lev_stealing_queue_type.h>
#include <uflib/logger/logger_type.h>

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Create a Chase–Lev work-stealing deque.
 *
 * The requested capacity is rounded up to the next power of two (minimum 2).
 * The deque grows by doubling on the owner side, so the requested capacity is
 * a starting point, not a hard bound.
 *
 * @param[in] requested_capacity  Initial capacity (rounded up; 0 → minimum 2).
 *
 * @return Opaque handle, or NULL on size overflow or allocation failure.
 *
 * @code{.c}
 * ChaseLevStealingQueue *q = ChaseLevStealingQueueCreate(0);
 * if (!q) { /* handle failure *\/ }
 * @endcode
 */
PUBLIC_API ChaseLevStealingQueue *
ChaseLevStealingQueueCreate(size_t requested_capacity);

/**
 * @brief Create a work-stealing deque that reports through @p logger_ptr.
 *
 * Behaves exactly as ChaseLevStealingQueueCreate(), and additionally reports
 * creation, each failure, and release through the supplied logger.  Push, Pop
 * and Steal are never logged — a deque built this way has the same concurrency
 * behaviour, and the same cache-line behaviour, as one built without a logger.
 *
 * The failures are worth distinguishing: this constructor can fail on size
 * overflow, on allocation, and on a platform whose pointer atomics are not
 * lock-free — the last of which means the structure cannot work at all, and is
 * reported at CRITICAL for that reason.
 *
 * The logger is **borrowed, not owned**.  The deque stores the pointer and
 * never destroys it, so the logger must outlive the deque: destroy the deque
 * before destroying the logger.  A deque destroyed after its logger would
 * report through a dangling handle.
 *
 * @param[in] requested_capacity  Initial capacity (rounded up; 0 → minimum 2).
 * @param[in] logger_ptr          Logger to report through, or NULL to report
 *                                nothing.  NULL gives exactly
 *                                ChaseLevStealingQueueCreate().
 *
 * @return Opaque handle, or NULL on size overflow or allocation failure.
 *
 * @code{.c}
 * UfLogger *log_ptr = NULL;
 * if (UfLoggerCreateWithDefaults(&log_ptr) != UF_LOGGER_STATUS_OK) { return NULL; }
 *
 * ChaseLevStealingQueue *q = ChaseLevStealingQueueCreateWithLogger(0, log_ptr);
 * if (!q) { UfLoggerDestroy(log_ptr); return NULL; }
 *
 * ChaseLevStealingQueueDestroy(q);   // the deque first —
 * UfLoggerDestroy(log_ptr);          // then the logger it borrowed
 * @endcode
 */
PUBLIC_API ChaseLevStealingQueue *
ChaseLevStealingQueueCreateWithLogger(size_t requested_capacity, UfLogger *logger_ptr);

/**
 * @brief Tear down the deque and free all storage (including retired rings).
 *
 * Requires external quiescence: no `Push`/`Pop`/`Steal` may be in flight.
 * NULL is safe (no-op).  Does not free any items — items are caller-owned
 * pointers.
 *
 * @param[in,out] q  Deque handle (NULL → no-op).
 */
PUBLIC_API void
ChaseLevStealingQueueDestroy(ChaseLevStealingQueue *q);

/* ── Core operations ───────────────────────────────────────────────────── */

/**
 * @brief Push one item onto the owner (bottom) end — LIFO.
 *
 * Owner-only.  Wait-free on the fast path (one relaxed slot store plus one
 * release store of `bottom`); allocation only when the ring must grow.
 *
 * @param[in,out] q     Deque handle (NULL → false).
 * @param[in]     item  Item pointer (NULL → false; NULL is not a legal item).
 *
 * @return true on success, false on NULL args or growth-allocation failure.
 *         On growth failure the logical contents are unchanged.
 */
PUBLIC_API bool
ChaseLevStealingQueuePush(ChaseLevStealingQueue *q, void *item);

/**
 * @brief Pop the newest item from the owner (bottom) end — LIFO.
 *
 * Owner-only.  Races a thief `Steal` on the last item: the owner and thief
 * arbitrate that single item via a seq-cst CAS on `top`, so the item is
 * delivered exactly once.
 *
 * @param[in,out] q       Deque handle (NULL → false).
 * @param[out]    item    Receives the popped item on success (NULL → false).
 *
 * @return true if an item was popped, false if empty or invalid args.
 */
PUBLIC_API bool
ChaseLevStealingQueuePop(ChaseLevStealingQueue *q, void **item);

/**
 * @brief Steal the oldest item from the top (thief) end — FIFO.
 *
 * Any thread (the owner excluded, though not enforced) may call this.  It is
 * the only contended path and is lock-free.
 *
 * @param[in,out] q       Deque handle (NULL → false).
 * @param[out]    item    Receives the stolen item on success (NULL → false).
 *
 * @return true if an item was stolen, false if empty or invalid args.
 */
PUBLIC_API bool
ChaseLevStealingQueueSteal(ChaseLevStealingQueue *q, void **item);

/* ── Introspection ─────────────────────────────────────────────────────── */

/**
 * @brief Current ring capacity.
 *
 * Safe to query concurrently; diagnostic only.  Grows by doubling, so this
 * is a power of two and increases over the deque lifetime.
 *
 * @param[in] q  Deque handle (NULL → 0).
 *
 * @return Current capacity in items.
 */
PUBLIC_API size_t
ChaseLevStealingQueueCapacity(const ChaseLevStealingQueue *q);

/**
 * @brief Approximate number of items currently in the deque.
 *
 * A relaxed `bottom - top` snapshot.  Under concurrent `Push`/`Steal` this
 * may be stale by the time the caller reads it; it is exact only under
 * quiescence.  It is a diagnostic, never a linearization point.
 *
 * @param[in] q  Deque handle (NULL → 0).
 *
 * @return Approximate item count (never negative).
 */
PUBLIC_API uint64_t
ChaseLevStealingQueueApproxSize(const ChaseLevStealingQueue *q);

#endif /* UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_CDT_CHASE_LEV_STEALING_QUEUE_H */
