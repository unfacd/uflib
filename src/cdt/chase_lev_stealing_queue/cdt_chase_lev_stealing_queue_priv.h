/**
 * @file cdt_chase_lev_stealing_queue_priv.h
 * @brief Chase–Lev work-stealing deque — private implementation details.
 *
 * Defines the internal `struct ChaseLevStealingQueue`, the ring
 * (`ChaseLevStealingQueueArray` / `ChaseLevStealingQueueSlot`), and the
 * cache-line layout.  THIS FILE IS NOT INSTALLED — it is private to the
 * module implementation and its tests.  Consumers must never include it.
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

#ifndef UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_CDT_CHASE_LEV_STEALING_QUEUE_PRIV_H
#define UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_CDT_CHASE_LEV_STEALING_QUEUE_PRIV_H

#include <uflib/standard_c_includes.h>

#include <uflib/cdt/chase_lev_stealing_queue/cdt_chase_lev_stealing_queue.h>
#include <uflib/cdt/chase_lev_stealing_queue/cdt_chase_lev_stealing_queue_type.h>
#include <uflib/logger/logger_type.h>

/** Cache-line size for padding to prevent false sharing. */
#ifndef UFLIB_CDT_CACHE_LINE_SIZE
#define UFLIB_CDT_CACHE_LINE_SIZE 64
#endif

/**
 * @brief One ring slot.  `_Atomic(void *)` so a thief may speculatively load
 * a slot concurrently with an owner store without a C data race.
 */
typedef struct ChaseLevStealingQueueSlot {
    _Atomic(void *) value; ///< Item pointer, or NULL for an unused cell.
} ChaseLevStealingQueueSlot;

/**
 * @brief A growable power-of-two ring.
 *
 * `slots[]` is a flexible array of `capacity` slots; `mask == capacity - 1`
 * so `index & mask` maps a monotonic logical index onto the ring.  `older`
 * links the previously-retired ring (freed only at destroy).
 */
typedef struct ChaseLevStealingQueueArray {
    size_t capacity;                        ///< Power-of-two slot count.
    size_t mask;                            ///< capacity - 1 (index mask).
    struct ChaseLevStealingQueueArray *older; ///< Retired predecessor (or NULL).
    ChaseLevStealingQueueSlot slots[];      ///< Flexible slot array.
} ChaseLevStealingQueueArray;

/**
 * @brief The deque instance.
 *
 * All fields are private; consumers see an opaque handle.  `top` and `bottom`
 * are monotonic `uint64_t` counters (slot index is `i & mask`).  Cache-line
 * layout (LP64):
 *
 *   Line 0: `top`     — thief CAS hotspot.
 *   Line 1: `bottom`  — owner publish / pop.
 *   Line 2: `array`   — thieves acquire-load every steal.
 *   Line 3: `capacity`— diagnostic (relaxed).
 *   Then `retired` / `cached_top` / `uf_logger` — owner-only, never touched by
 *   thieves, and past the last padded line so adding to the group shifts no
 *   existing offset.
 *
 * `uf_logger` is borrowed, never owned, and written once at create.  It sits in
 * the owner-only group because no thief and no hot path reads it: it is
 * consulted only when the handle is created or released.
 */
struct ChaseLevStealingQueue {
    /* ── Line 0: thief CAS hotspot ──────────────────────────────────── */
    _Atomic uint64_t top;
    unsigned char _pad_top_[UFLIB_CDT_CACHE_LINE_SIZE - sizeof(uint64_t)];

    /* ── Line 1: owner publish / pop ─────────────────────────────────── */
    _Atomic uint64_t bottom;
    unsigned char _pad_bottom_[UFLIB_CDT_CACHE_LINE_SIZE - sizeof(uint64_t)];

    /* ── Line 2: thieves acquire-load every steal ───────────────────── */
    _Atomic(ChaseLevStealingQueueArray *) array;
    unsigned char _pad_array_[UFLIB_CDT_CACHE_LINE_SIZE - sizeof(void *)];

    /* ── Line 3: diagnostic capacity ────────────────────────────────── */
    _Atomic size_t capacity;
    unsigned char _pad_capacity_[UFLIB_CDT_CACHE_LINE_SIZE - sizeof(size_t)];

    /* ── Owner-only (thieves must not bounce these lines) ───────────── */
    ChaseLevStealingQueueArray *retired;   ///< Head of the retained-ring chain.
    uint64_t cached_top;                   ///< Cached lower bound on top.
    UfLogger *uf_logger;                   ///< Borrowed diagnostic sink; NULL = silent.
};

/* The last-item arbitration CASes the array pointer; a non-lock-free pointer
 * atomic would silently break the lock-free guarantee. */
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "requires lock-free pointer atomics");

#endif /* UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_CDT_CHASE_LEV_STEALING_QUEUE_PRIV_H */
