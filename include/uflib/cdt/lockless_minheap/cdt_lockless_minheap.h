/**
 * @file cdt_lockless_minheap.h
 * @brief Lock-free integer min-priority queue — public API.
 *
 * A concurrent priority queue over inline `int64_t` keys.  It is NOT a
 * binary heap: it is a **Harris lock-free ordered list**, the standard
 * technique for a lock-free priority queue (a binary heap's parent/child
 * sift is a multi-word update that C17 atomics cannot linearize portably).
 *
 * The invariant is: the list is sorted by key, and `delmin` removes the
 * leftmost (smallest) live node.  Nodes are marked-then-unlinked (Harris
 * tombstone discipline); physical unlink is done cooperatively — whichever
 * walker notices a marked node unlinks and retires it, so a node is retired
 * exactly once.
 *
 * Concurrency model (C17 atomics):
 *
 * - `insert_i64` linearizes at the `release` CAS that links the new node
 *   after its predecessor.
 * - `delmin_i64` linearizes at the `acq_rel` CAS that marks the minimum's
 *   successor link.
 * - `size_approx` is a relaxed counter — telemetry, not part of the
 *   linearization.  It is exact only after a quiescent drain.
 *
 * Reclamation: retired nodes are handed to an intrusive
 * LocklessTreiberStack and freed only by LocklessMinHeapReclaim() /
 * LocklessMinHeapDestroy().  Reclaim is **quiescent** — call it only after
 * every thread has left insert/delmin (after a join), never concurrently.
 *
 * Not provided in v1: `peek` (a lock-free min() without removal needs a
 * pin/hazard pointer to be safe), and decrease-key (use insert-new +
 * discard-stale instead).
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

#ifndef UFLIB_CDT_LOCKLESS_MINHEAP_CDT_LOCKLESS_MINHEAP_H
#define UFLIB_CDT_LOCKLESS_MINHEAP_CDT_LOCKLESS_MINHEAP_H

#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>
#include <uflib/cdt/lockless_minheap/cdt_lockless_minheap_type.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Status codes ───────────────────────────────────────────────────────── */

enum {
    LOCKLESS_MINHEAP_OK        =  0,   /**< Success. */
    LOCKLESS_MINHEAP_ERR_OOM   = -1,   /**< Allocation failure. */
    LOCKLESS_MINHEAP_ERR_INVAL = -2    /**< Invalid argument (NULL handle). */
};

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/**
 * @brief Create an empty lock-free min-PQ.
 *
 * @return Opaque handle, or NULL on allocation failure.
 *
 * @code{.c}
 * LocklessMinHeap *pq = LocklessMinHeapCreate();
 * if (!pq) { // handle allocation failure }
 * LocklessMinHeapDestroy(pq);
 * @endcode
 */
PUBLIC_API LocklessMinHeap *
LocklessMinHeapCreate(void);

/**
 * @brief Tear down the queue and free all internal storage.
 *
 * Frees both the live nodes and the retired nodes awaiting reclamation, then
 * the handle itself.  Caller-owned values are NOT freed.  NULL is a no-op.
 *
 * Must only be called when no other thread is inside insert/delmin — it
 * performs the quiescent reclamation internally.
 *
 * @param h  Queue handle (NULL → no-op).
 */
PUBLIC_API void
LocklessMinHeapDestroy(LocklessMinHeap *h);

/* ── Insert ─────────────────────────────────────────────────────────────── */

/**
 * @brief Insert an inline `int64_t` key with a caller-owned value.
 *
 * Concurrent with any number of other insert/delmin calls.  Equal keys are
 * stored and extracted (the queue is a multiset; the relative order of equal
 * keys is unspecified).
 *
 * @param h      Queue handle.
 * @param key    Integer key (stored by value).
 * @param value  Caller-owned pointer (not copied, not freed).
 * @return LOCKLESS_MINHEAP_OK, LOCKLESS_MINHEAP_ERR_OOM, or
 *         LOCKLESS_MINHEAP_ERR_INVAL (NULL handle).
 */
PUBLIC_API int
LocklessMinHeapInsertI64(LocklessMinHeap *h, int64_t key, void *value);

/* ── Remove ─────────────────────────────────────────────────────────────── */

/**
 * @brief Remove and return the current minimum.
 *
 * The returned key is the minimum of the live set at the call's
 * linearization point — under concurrency the extraction order is NOT a
 * global sort, only "this key was ≤ every key whose insert completed before
 * this delmin linearized".  After a quiescent drain the remaining sequence
 * is sorted.
 *
 * @param h      Queue handle.
 * @param key    Out: removed int64 key (may be NULL).
 * @param value  Out: removed value (may be NULL).
 * @return 1 if a minimum was removed, 0 if empty or h is NULL.
 */
PUBLIC_API int
LocklessMinHeapDelminI64(LocklessMinHeap *h, int64_t *key, void **value);

/* ── Introspection ──────────────────────────────────────────────────────── */

/**
 * @brief Current approximate element count.
 *
 * Relaxed telemetry — under concurrent mutation the value may be stale and
 * is only exact after a quiescent drain.  It is NOT part of the linearization
 * and must not be used to gate control flow that depends on exactness.
 *
 * @param h  Queue handle (NULL → 0).
 * @return Approximate live-element count.
 */
PUBLIC_API size_t
LocklessMinHeapSizeApprox(const LocklessMinHeap *h);

/* ── Reclamation ────────────────────────────────────────────────────────── */

/**
 * @brief Steal and free all retired nodes.
 *
 * Drains the retire stack and frees each node exactly once.  Call only at a
 * quiescent point — after every thread has left insert/delmin (the intended
 * use is after pthread_join).  Calling it concurrently with insert/delmin is
 * unsafe (use-after-reclaim).  destroy() always reclaims internally.
 *
 * @param h  Queue handle (NULL → no-op).
 */
PUBLIC_API void
LocklessMinHeapReclaim(LocklessMinHeap *h);

/* ── Introspection (JSON) ───────────────────────────────────────────────── */

/**
 * @brief Describe the queue's state as a JSON string.
 *
 * Composes a best-effort snapshot: the relaxed size counter, a walk of the
 * live ordered list (node count plus one entry per node with key and value
 * address), and a reclamation note.  All reads are relaxed snapshots, so
 * figures are approximate under concurrent mutation — introspection, not a
 * transactional view.  Call it only when no reclaim is in flight.
 *
 * If @p provided is NULL, a fresh BufferDescriptor is allocated (the caller
 * must free it with BufferDescriptorRelease() + free()); otherwise the
 * caller's descriptor is appended to.  Returns @p provided, or NULL only if
 * a fresh descriptor could not be allocated.
 *
 * @param h         Queue handle (NULL → emits `{"error":"null handle"}`).
 * @param provided  Optional caller-owned BufferDescriptor (NULL → allocate).
 * @return The populated BufferDescriptor.
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 512);
 * DescribeLocklessMinHeap(pq, &bd);
 * printf("%s\n", bd.data);
 * BufferDescriptorRelease(&bd);
 * @endcode
 */
PUBLIC_API BufferDescriptor *
DescribeLocklessMinHeap(LocklessMinHeap *h, BufferDescriptor *provided);

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_CDT_LOCKLESS_MINHEAP_CDT_LOCKLESS_MINHEAP_H */
