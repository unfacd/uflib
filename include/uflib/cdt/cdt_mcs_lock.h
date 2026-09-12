/**
 * @file cdt_mcs_lock.h
 * @brief MCS (Mellor-Crummey & Scott) queue-based spinlock — FIFO fair,
 *        local spinning, O(1) space per lock, C11/C++17 atomics.
 *
 * Each waiter spins on a field in its **own** node, eliminating the
 * cache-line ping-pong of a naive test-and-set spinlock.  Handoff is
 * strict FIFO — no starvation.  The lock itself is a single pointer
 * (8 bytes); per-acquire nodes are caller-owned (stack-local is the
 * recommended pattern).
 *
 * ## Choosing between MCS, Anderson, and pthread_spinlock
 *
 * | Criterion                  | MCS (this module)              | Anderson                     | pthread_spinlock_t            |
 * |----------------------------|--------------------------------|------------------------------|-------------------------------|
 * | Fairness                   | Strict FIFO (no starvation)    | Strict FIFO (no starvation)  | Unfair (may starve)           |
 * | Space per lock             | O(1) — 8 bytes                 | O(N) — array of N slots      | O(1)                          |
 * | Must know thread count?    | No                             | Yes — array sized upfront    | No                            |
 * | Per-acquire space          | One node (16 or 128 bytes)     | One slot (reused from array) | None                          |
 * | Cache behaviour            | Local spin, one-time link cost | Per-slot spin, less bounce   | Single-location ping-pong     |
 * | Node lifetime management   | Caller-managed per critical section | Array outlives the lock | None                       |
 * | Reentrant                  | No                             | No                           | No                            |
 * | Standard                   | Hand-rolled                    | Hand-rolled (from CK)        | POSIX, portable               |
 *
 * **Rule of thumb:** MCS is the default choice when thread count is unknown
 * or unbounded.  Anderson is lighter-weight when the maximum thread count is
 * small and known.  pthread_spinlock is simplest for uncontended or lightly
 * contended paths.
 *
 * ## Lifecycle
 *
 * @code{.c}
 * #include <uflib/cdt/cdt_mcs_lock.h>
 *
 * // One lock instance (embed in your struct or allocate statically):
 * struct SpinlockMcs lock = SPINLOCK_MCS_INITIALIZER;
 * // Or at runtime:  SpinlockMcsInit(&lock);
 *
 * // Each thread, for each critical section:
 * void do_work(struct SpinlockMcs *lock_ptr) {
 *     struct SpinlockMcsNode node;        // stack-local — automatic lifetime
 *     SpinlockMcsNodeInit(&node);
 *     SpinlockMcsLock(lock_ptr, &node);
 *     // ... critical section ...
 *     SpinlockMcsUnlock(lock_ptr, &node);
 * }
 * @endcode
 *
 * ## The one rule that matters
 *
 * The node you pass to `SpinlockMcsLock()` must stay alive and untouched by
 * anyone else for the entire lock→unlock window.  Stack-allocating a fresh
 * node per critical section (as above) is the simplest safe pattern — do not
 * share one node across threads or reuse it before unlock returns.
 *
 * ## Complete standalone example
 *
 * @code{.c}
 * // Compile:  gcc -O2 -Wall -Wextra -pthread -std=c11 -o mcs_demo mcs_demo.c
 * // Run:      ./mcs_demo
 *
 * #include <uflib/cdt/cdt_mcs_lock.h>
 * #include <stdio.h>
 * #include <stdlib.h>
 * #include <pthread.h>
 *
 * #define NUM_THREADS   8
 * #define OPS_PER_THREAD 5000
 *
 * struct Shared {
 *     struct SpinlockMcs lock;
 *     long               counter;
 * };
 *
 * static struct Shared *g_shared;
 *
 * static void *
 * worker(void *arg)
 * {
 *     (void)arg;
 *     for (int i = 0; i < OPS_PER_THREAD; i++) {
 *         struct SpinlockMcsNode node;
 *         SpinlockMcsNodeInit(&node);
 *         SpinlockMcsLock(&g_shared->lock, &node);
 *         g_shared->counter++;
 *         SpinlockMcsUnlock(&g_shared->lock, &node);
 *     }
 *     return NULL;
 * }
 *
 * int
 * main(void)
 * {
 *     g_shared = calloc(1, sizeof(*g_shared));
 *     SpinlockMcsInit(&g_shared->lock);
 *
 *     pthread_t threads[NUM_THREADS];
 *     for (int i = 0; i < NUM_THREADS; i++)
 *         pthread_create(&threads[i], NULL, worker, NULL);
 *     for (int i = 0; i < NUM_THREADS; i++)
 *         pthread_join(threads[i], NULL);
 *
 *     long expected = (long)NUM_THREADS * OPS_PER_THREAD;
 *     printf("shared_counter = %ld, expected = %ld -> %s\n",
 *            g_shared->counter, expected,
 *            g_shared->counter == expected ? "PASS" : "FAIL");
 *
 *     free(g_shared);
 *     return g_shared->counter == expected ? 0 : 1;
 * }
 * @endcode
 *
 * Algorithm: J. M. Mellor-Crummey and M. L. Scott, "Algorithms for Scalable
 * Synchronization on Shared-Memory Multiprocessors," ACM TOCS 9(1), 1991.
 *
 * This is a clean-room C11 implementation using `<stdatomic.h>`.  It is NOT
 * a copy of Linux's kernel/locking/qspinlock.c (which is GPL-2.0-only and
 * depends on kernel-internal primitives unavailable in userspace).
 */

/*
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

#ifndef UFLIB_CDT_MCS_LOCK_H
#define UFLIB_CDT_MCS_LOCK_H

/* ── Shared CDT spinlock definitions ─────────────────────────────────────── */

#include <uflib/cdt/cdt_spinlock_defs.h>

/* ── Types ───────────────────────────────────────────────────────────────── */

/*!
 * Per-thread node in the MCS lock queue.
 *
 * The caller supplies one node per critical section (stack allocation
 * is the recommended pattern).  The node must outlive the acquire→release
 * pair and must not be shared across threads.
 *
 * @warning Reusing a node without calling SpinlockMcsNodeInit() first
 *          corrupts the queue linkage.  Either declare a fresh node each
 *          time (stack-local inside the critical-section scope) or
 *          explicitly re-initialise before every acquire.
 */
struct SpinlockMcsNode {
    _Atomic(struct SpinlockMcsNode *) next;  ///< Successor in queue (written by successor, read by us)
    _Atomic(bool)                    locked; ///< Spin flag: true = waiting, false = handed off
};
typedef struct SpinlockMcsNode SpinlockMcsNode;

/*!
 * Cache-line-padded variant of `SpinlockMcsNode`.
 *
 * Isolates `next` and `locked` onto separate 64-byte cache lines so that
 * a successor writing `pred->next` (to link in) never dirties the cache
 * line containing `locked` (which the predecessor is spinning on).
 *
 * Trade-off: 128 bytes per node vs 16 bytes for the un-padded variant.
 * Use when the one-time link-in cache miss is measurable on your workload.
 */
struct SpinlockMcsNodePadded {
    _Atomic(struct SpinlockMcsNodePadded *) next;  ///< Successor pointer (cache line 0)
    char                                    pad1[CDT_CACHELINE_SZ - 8]; ///< pad to 64 bytes
    _Atomic(bool)                           locked; ///< Spin flag (cache line 1)
    char                                    pad2[CDT_CACHELINE_SZ - 1]; ///< pad to 128 bytes
};
typedef struct SpinlockMcsNodePadded SpinlockMcsNodePadded;

/*!
 * Control block for one MCS lock instance.
 *
 * The lock is a single atomic pointer (8 bytes on LP64).  Embed it in
 * whatever struct needs protection — no pre-allocation, no arrays, no
 * init-time sizing.
 */
struct SpinlockMcs {
    _Atomic(struct SpinlockMcsNode *) tail;  ///< Tail of the waiter queue (NULL when unlocked)
};
typedef struct SpinlockMcs SpinlockMcs;

/* ── Static initializer ──────────────────────────────────────────────────── */

/*!
 * Static initializer for `SpinlockMcs`.
 *
 * @code{.c}
 * struct SpinlockMcs lock = SPINLOCK_MCS_INITIALIZER;
 * // lock is immediately usable — no runtime init call required
 * @endcode
 */
#define SPINLOCK_MCS_INITIALIZER { .tail = NULL }

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * @brief Initialise an MCS spinlock at runtime.
 *
 * Call once before any thread may call SpinlockMcsLock() on this lock.
 * If the lock was statically initialised with SPINLOCK_MCS_INITIALIZER,
 * this call is redundant but harmless.
 *
 * @param lock_ptr  Pre-allocated control block (zero-filled or uninitialised).
 *
 * @code{.c}
 * struct SpinlockMcs lock;
 * SpinlockMcsInit(&lock);
 * @endcode
 */
static inline void
SpinlockMcsInit(struct SpinlockMcs *lock_ptr)
{
    if (!lock_ptr)
        return;
    atomic_init(&lock_ptr->tail, NULL);
}

/**
 * @brief Initialise (or reset) an MCS node before acquire.
 *
 * Must be called on a node before each call to SpinlockMcsLock().
 * For stack-local nodes declared fresh before each critical section,
 * this initialises the node to a clean state.  For reused nodes,
 * this clears state from the previous acquire→release cycle.
 *
 * @param node_ptr  Caller-owned node to initialise.
 *
 * @code{.c}
 * struct SpinlockMcsNode node;
 * SpinlockMcsNodeInit(&node);
 * SpinlockMcsLock(&lock, &node);
 * // critical section
 * SpinlockMcsUnlock(&lock, &node);
 * @endcode
 */
static inline void
SpinlockMcsNodeInit(struct SpinlockMcsNode *node_ptr)
{
    if (!node_ptr)
        return;
    atomic_init(&node_ptr->next, NULL);
    atomic_init(&node_ptr->locked, false);
}

/**
 * @brief Acquire the MCS spinlock (blocking).
 *
 * Atomically enqueues the caller's node at the tail of the waiter queue.
 * If the queue was empty (no predecessor), the lock is acquired
 * immediately — the fast path.  If a predecessor exists, the caller spins
 * on its own node's `locked` field until handed off by the predecessor.
 *
 * Acquisition is strictly FIFO — threads are served in the order they
 * arrived.
 *
 * @param lock_ptr  Initialised lock.
 * @param self_ptr  Caller-owned node.  Must have been initialised with
 *                  SpinlockMcsNodeInit() and must remain in scope until
 *                  the matching SpinlockMcsUnlock() returns.
 *
 * @warning The node must not be shared across threads.  Each thread
 *          competing for the same lock needs its own node.
 *
 * @code{.c}
 * struct SpinlockMcsNode node;
 * SpinlockMcsNodeInit(&node);
 * SpinlockMcsLock(&lock, &node);
 * // ... critical section ...
 * SpinlockMcsUnlock(&lock, &node);
 * @endcode
 */
static inline void
SpinlockMcsLock(struct SpinlockMcs *lock_ptr, struct SpinlockMcsNode *self_ptr)
{
    struct SpinlockMcsNode *pred_ptr;

    atomic_store_explicit(&self_ptr->next, NULL, UFLIB_MO_RELAXED);

    pred_ptr = atomic_exchange_explicit(&lock_ptr->tail, self_ptr,
                                         UFLIB_MO_ACQ_REL);
    if (pred_ptr != NULL) {
        /*
         * Mark ourselves as waiting.  This store is not yet visible to the
         * predecessor — the release on pred->next below publishes it.
         */
        atomic_store_explicit(&self_ptr->locked, true, UFLIB_MO_RELAXED);

        /*
         * Publish our node to the predecessor.  The release ensures the
         * predecessor sees locked=true before it attempts the handoff.
         */
        atomic_store_explicit(&pred_ptr->next, self_ptr, UFLIB_MO_RELEASE);

        /*
         * Spin on our own cache line.  The acquire syncs with the
         * predecessor's release-store to our locked field, giving us
         * visibility of all stores made in the predecessor's critical
         * section.
         */
        while (atomic_load_explicit(&self_ptr->locked, UFLIB_MO_ACQUIRE)) {
            SPINWAIT();
        }
    }
    /* else: queue was empty — lock acquired immediately (fast path) */
}

/**
 * @brief Release the MCS spinlock.
 *
 * If a successor is waiting, hands off the lock by clearing the successor's
 * `locked` flag.  If no successor is visible, atomically swings the tail
 * pointer back to NULL — if a successor arrives mid-swing, spins briefly
 * until the successor finishes linking, then hands off.
 *
 * @param lock_ptr  Initialised lock.
 * @param self_ptr  The exact same node pointer passed to the matching
 *                  SpinlockMcsLock() call.  Must not be NULL.
 *
 * @warning Passing a node that does not belong to the calling thread
 *          (or a node not currently holding the lock) silently corrupts
 *          the queue.
 *
 * @code{.c}
 * SpinlockMcsUnlock(&lock, &node);
 * // node can now be reused (after re-init) or go out of scope
 * @endcode
 */
static inline void
SpinlockMcsUnlock(struct SpinlockMcs *lock_ptr, struct SpinlockMcsNode *self_ptr)
{
    struct SpinlockMcsNode *succ_ptr;

    succ_ptr = atomic_load_explicit(&self_ptr->next, UFLIB_MO_ACQUIRE);

    if (succ_ptr == NULL) {
        struct SpinlockMcsNode *expected_ptr = self_ptr;

        /*
         * Try to swing tail back to NULL.  If it's still us, we were the
         * last waiter and the lock is now free.
         *
         * Success (acq_rel): flushes our critical-section stores before
         *   tail becomes NULL, so the next thread to acquire sees them.
         * Failure (acquire): a successor exchanged into tail but hasn't
         *   finished writing pred->next yet — we must spin for it.
         */
        if (atomic_compare_exchange_strong_explicit(
                &lock_ptr->tail, &expected_ptr, NULL,
                UFLIB_MO_ACQ_REL, UFLIB_MO_ACQUIRE)) {
            return; /* last waiter — lock is now free */
        }

        /*
         * Mid-link window: the successor won the XCHG (became tail) but
         * hasn't stored to pred->next yet.  Spin briefly — the successor
         * must complete the link (it's only a few instructions away).
         */
        do {
            succ_ptr = atomic_load_explicit(&self_ptr->next,
                                             UFLIB_MO_ACQUIRE);
        } while (succ_ptr == NULL);
    }

    /*
     * Hand off to successor.  The release ensures all our critical-section
     * stores are globally visible before the successor sees locked=false
     * and enters its own critical section.
     */
    atomic_store_explicit(&succ_ptr->locked, false, UFLIB_MO_RELEASE);
}

/**
 * @brief Query whether the lock is currently held or waited on.
 *
 * @param lock_ptr  Initialised lock.
 * @return true if any thread holds or is waiting for the lock, false if free.
 *
 * @note This is a snapshot — the answer may be stale by the time the
 *       caller acts on it.  Useful for assertions and diagnostics only.
 *
 * @code{.c}
 * assert(!SpinlockMcsLocked(&lock));  // lock must be free at this point
 * @endcode
 */
static inline bool
SpinlockMcsLocked(struct SpinlockMcs *lock_ptr)
{
    return atomic_load_explicit(&lock_ptr->tail, UFLIB_MO_ACQUIRE) != NULL;
}

#endif /* UFLIB_CDT_MCS_LOCK_H */
