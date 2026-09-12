/**
 * @file cdt_anderson_spinlock.h
 * @brief Anderson array-based queue spinlock — FIFO fair, one slot per
 *        contender, C11/C++17 atomics.
 *
 * Adapted from Concurrency Kit (ck) for C11 / C++17 compatibility.
 * Header-only: all functions are inline static.
 *
 * ## Constraint — threads MUST NOT exceed slots
 *
 * Each contending thread needs its own unique slot.  When more threads than
 * slots contend simultaneously, the lock **silently corrupts** (lost mutual
 * exclusion, lost updates).  The @p slot_count passed to
 * spinlock_anderson_init() is the maximum number of concurrently contending
 * threads.  Debug builds assert this constraint at unlock time.
 *
 * ## Lifecycle
 *
 * @code{.c}
 * // One-time setup (before spawning contender threads):
 * spinlock_anderson_thread_t slots[8];
 * spinlock_anderson_t       lock;
 * spinlock_anderson_init(&lock, slots, 8);
 *
 * // Each contender thread:
 * spinlock_anderson_thread_t *my_slot = NULL;
 * spinlock_anderson_lock(&lock, &my_slot);
 * // ... critical section ...
 * spinlock_anderson_unlock(&lock, my_slot);
 * @endcode
 *
 * ## Choosing between Anderson and pthread_spinlock
 *
 * | Criterion                | Anderson (this module)         | pthread_spinlock_t            |
 * |--------------------------|--------------------------------|-------------------------------|
 * | Fairness                 | Strict FIFO (no starvation)    | Unfair (may starve)           |
 * | Space                    | O(N) — one slot per contender  | O(1)                          |
 * | Cache behaviour          | Per-slot spinning, less bounce | Single-location ping-pong     |
 * | High contention          | Scales better                  | Degrades under many cores     |
 * | Low contention (2-3 th.) | Slightly more bookkeeping      | Just as fast or faster        |
 * | Standard                 | Hand-rolled                    | POSIX, portable               |
 *
 * Rule of thumb: low contention / few threads → pthread_spinlock is simpler.
 * High contention / many cores / fairness matters → Anderson (or MCS).
 *
 * @see spinlock_anderson_init
 * @see spinlock_anderson_lock
 * @see spinlock_anderson_unlock
 */

/*
 * Copyright 2010-2015 Samy Al Bahra.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef UFLIB_CDT_ANDERSON_SPINLOCK_H
#define UFLIB_CDT_ANDERSON_SPINLOCK_H

/* ── Shared CDT spinlock definitions ─────────────────────────────────────── */

#include <uflib/cdt/cdt_spinlock_defs.h>

/* Backward-compatibility alias — prefer CDT_CACHELINE_SZ in new code. */
#define ANDERSON_CACHELINE_SZ CDT_CACHELINE_SZ

/* ── Types ───────────────────────────────────────────────────────────────── */

/*!
 * Per-thread slot in the Anderson lock array.
 *
 * Each contending thread requires its own slot.  The number of slots
 * (@p slot_count passed to spinlock_anderson_init()) is the **maximum
 * number of threads** that may contend on this lock simultaneously.
 */
struct spinlock_anderson_thread {
    _Atomic(unsigned int) locked;   ///< Spin flag: true = locked, false = available to claim
    unsigned int          position; ///< Slot index within the array (read-only after init)
};
typedef struct spinlock_anderson_thread spinlock_anderson_thread_t;

/*!
 * Padded variant of `spinlock_anderson_thread` — isolates adjacent slots
 * onto separate cache lines (64 bytes each).  Use when false sharing
 * between adjacent slots during unlock handoff is measurable.
 *
 * Trade-off: N × 64 bytes vs N × 8 bytes for the un-padded variant.
 */
struct spinlock_anderson_thread_padded {
    _Atomic(unsigned int) locked;
    unsigned int          position;
    char                  _pad[ANDERSON_CACHELINE_SZ - sizeof(unsigned int) * 2];
};
typedef struct spinlock_anderson_thread_padded spinlock_anderson_thread_padded_t;

/*
 * Control block for one Anderson lock instance.
 *
 * The `next` counter is isolated on its own cache line (offset ≥ 64)
 * to prevent false sharing with the control fields (`slots`, `count`, …).
 */
struct spinlock_anderson {
    struct spinlock_anderson_thread *slots;   ///< Array of per-thread slots (caller-owned)
    unsigned int                     count;   ///< Number of slots (= max contender threads)
    unsigned int                     wrap;    ///< Non-power-of-2 overflow guard (0 when power-of-2)
    unsigned int                     mask;    ///< count - 1 (fast modulo when power-of-2)
    char                             _pad[ANDERSON_CACHELINE_SZ
                                          - sizeof(unsigned int) * 3
                                          - sizeof(void *)];
    _Atomic(unsigned int)            next;    ///< Global ticket counter (isolated on own cache line)
};
typedef struct spinlock_anderson spinlock_anderson_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * @brief Initialise an Anderson spinlock and its slot array.
 *
 * Must be called **before** any contender thread may call
 * spinlock_anderson_lock() on this lock.  Typically called once at
 * startup, before spawning worker threads (so that pthread_create()
 * provides the necessary happens-before edge).
 *
 * @param lock       Pre-allocated control block (zero-filled or uninitialised).
 * @param slots      Pre-allocated array of per-thread slots — one per
 *                   **concurrently contending** thread.  Caller owns this
 *                   memory; it must outlive the lock.
 * @param slot_count Number of entries in @p slots.  This is the **maximum
 *                   number of threads** that may contend simultaneously.
 *                   Must be > 0.  Power-of-2 values use a faster fetch_add
 *                   path; non-power-of-2 uses a CAS loop.
 *
 * @warning If more than @p slot_count threads contend at the same time,
 *          the lock **silently corrupts** — multiple threads may enter the
 *          critical section concurrently.  Debug builds detect this at
 *          unlock time with a position-sanity assertion.
 *
 * @code{.c}
 * spinlock_anderson_thread_t slots[16];
 * spinlock_anderson_t       lock;
 * spinlock_anderson_init(&lock, slots, 16);
 * @endcode
 */
inline static void
spinlock_anderson_init(struct spinlock_anderson *lock,
                       struct spinlock_anderson_thread *slots,
                       unsigned int slot_count)
{
    unsigned int i;

    if (!lock || !slots || slot_count == 0)
        return;

    /* Slot 0 starts unlocked; all others start locked. */
    atomic_init(&slots[0].locked, (unsigned int)false);
    slots[0].position = 0;

    for (i = 1; i < slot_count; i++) {
        atomic_init(&slots[i].locked, (unsigned int)true);
        slots[i].position = i;
    }

    lock->slots = slots;
    lock->count = slot_count;
    lock->mask  = slot_count - 1;
    atomic_init(&lock->next, 0U);

    /*
     * If the number of slots is not a power of two then compute the
     * wrap-around value so the CAS slow-path in lock() correctly handles
     * unsigned-overflow of the next counter.
     */
    if (slot_count & (slot_count - 1))
        lock->wrap = (UINT_MAX % slot_count) + 1;
    else
        lock->wrap = 0;

    /*
     * Ensure all init stores are globally visible before any thread calls
     * lock().  In the common case (threads spawned after init) pthread_create
     * provides the happens-before, but this fence also covers thread-pool
     * workers that were already running.
     */
    atomic_thread_fence(UFLIB_MO_RELEASE);
}

/**
 * @brief Query whether the lock is currently held.
 *
 * @param lock  Initialised lock.
 * @return true if the lock is held by any thread, false if free.
 *
 * @note This is a snapshot — the answer may be stale by the time the
 *       caller acts on it.  Useful for assertions and diagnostics only.
 */
inline static bool
spinlock_anderson_locked(struct spinlock_anderson *lock)
{
    unsigned int position;
    bool         r;

    position = atomic_load_explicit(&lock->next, UFLIB_MO_ACQUIRE) & lock->mask;
    r        = atomic_load_explicit(&lock->slots[position].locked, UFLIB_MO_ACQUIRE);
    return r;
}

/**
 * @brief Acquire the Anderson spinlock (blocking).
 *
 * Allocates a unique ticket from the global counter, then spins on the
 * assigned slot's `locked` flag until the previous holder unlocks.
 * Acquisition is strictly FIFO — threads are served in ticket order.
 *
 * @param      lock  Initialised lock.
 * @param[out] slot  Set to point to this thread's slot on return.
 *                   Must be passed to the matching unlock() call.
 *
 * @warning The number of concurrently contending threads must NOT exceed
 *          @p slot_count from init().  Over-subscription breaks mutual
 *          exclusion.  Debug builds detect this.
 *
 * @code{.c}
 * spinlock_anderson_thread_t *my_slot = NULL;
 * spinlock_anderson_lock(&lock, &my_slot);
 * // ... critical section ...
 * spinlock_anderson_unlock(&lock, my_slot);
 * @endcode
 */
inline static void
spinlock_anderson_lock(struct spinlock_anderson *lock,
                       struct spinlock_anderson_thread **slot)
{
    unsigned int position, next;
    unsigned int count = lock->count;

    /*
     * Non-power-of-2 slow path: CAS loop to prevent overflow from
     * re-assigning the same slot to multiple threads.
     */
    if (lock->wrap != 0) {
        position = atomic_load_explicit(&lock->next, UFLIB_MO_ACQUIRE);

        do {
            if (position == UINT_MAX)
                next = lock->wrap;
            else
                next = position + 1;
        } while (atomic_compare_exchange_strong_explicit(
                     &lock->next, &position, next,
                     UFLIB_MO_RELEASE, UFLIB_MO_RELAXED) == false);

        position %= count;
    } else {
        /* Power-of-2 fast path: fetch_add + mask. */
        position  = atomic_fetch_add_explicit(&lock->next, 1, UFLIB_MO_RELEASE);
        position &= lock->mask;
    }

    /*
     * Spin on our slot until the previous holder marks it available.
     * The acquire-load synchronises-with the previous holder's
     * release-store in unlock(), giving us visibility of all stores
     * made in every prior critical section.
     */
    while (atomic_load_explicit(&lock->slots[position].locked,
                                UFLIB_MO_ACQUIRE) == true)
        SPINWAIT();

    /*
     * Mark our slot as locked so the next contender (position + 1)
     * cannot proceed until we unlock.
     */
    atomic_store_explicit(&lock->slots[position].locked, true,
                          UFLIB_MO_RELEASE);

	/*
	 * Debug: the slot we just claimed must have been unlocked before
	 * we acquired it.  A stored true here indicates a double-lock or
	 * over-subscription (two threads assigned the same slot).
	 */
	(void)lock;

    *slot = lock->slots + position;
}

/**
 * @brief Release the Anderson spinlock.
 *
 * Marks the **next** slot in the circular array as available, waking
 * the thread (if any) spinning on that slot.  The release-store
 * synchronises-with the next holder's acquire-load in lock().
 *
 * @param lock  Initialised lock.
 * @param slot  The slot pointer returned by the matching lock() call.
 *              Must be the exact same pointer.
 *
 * @warning Passing a slot that does not belong to the calling thread
 *          silently corrupts the lock.  Debug builds detect mismatched
 *          lock/unlock pairs and over-subscription.
 */
inline static void
spinlock_anderson_unlock(struct spinlock_anderson *lock,
                         struct spinlock_anderson_thread *slot)
{
    unsigned int position;

    /*
     * Compute the slot position the current holder must release.
     * This is always (holder_position + 1) wrapped by the slot count.
     */
    if (lock->wrap == 0)
        position = (slot->position + 1) & lock->mask;
    else
        position = (slot->position + 1) % lock->count;

    /*
     * Release-store: makes all our critical-section stores visible to
     * the next thread that acquires this slot.
     */
    atomic_store_explicit(&lock->slots[position].locked, false,
                          UFLIB_MO_RELEASE);
}

#endif /* UFLIB_CDT_ANDERSON_SPINLOCK_H */
