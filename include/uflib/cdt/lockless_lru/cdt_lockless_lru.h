/**
 * @file cdt_lockless_lru.h
 * @brief Lockless approximate LRU cache — public API.
 *
 * This is a bounded, evicting cache using CLOCK (second-chance)
 * approximate LRU, built on C11/C17 <stdatomic.h>.  Reads (Get) are
 * genuinely lock-free/wait-free — no CAS, no spinning, bounded number of
 * steps.  Writes (Set/Remove/eviction) use a per-SLOT seqlock: a single
 * CAS claims one slot for a few atomic stores, then releases it.  Two
 * threads writing to different slots never block each other.
 *
 * IMPORTANT — read before using:
 *
 * 1. This is NOT exact LRU.  Exact global recency ordering with lock-free
 *    bounded eviction isn't a practically solved problem — the moment you
 *    need "find the single globally-oldest item" without a lock, you're
 *    fighting an ordering problem atomics don't solve efficiently.
 *    CLOCK/second-chance is the real-world answer: same asymptotic hit-rate
 *    behaviour as LRU in practice (it's what Linux's page cache and many
 *    production caches use), but it does not guarantee "the exact
 *    least-recently-used item" is always the one evicted.
 *
 * 2. Keys are uint64_t.  If you need string/composite keys, hash them
 *    yourself before calling.  Using a 64-bit hash as identity carries a
 *    (very small, but nonzero) birthday-bound collision risk; if that's
 *    unacceptable for your use case, store full keys alongside and compare
 *    them — not implemented here to keep this readable.
 *
 * 3. Capacity bound: max_items is a TARGET, not a hard ceiling.  Under
 *    concurrent insertion the cache may temporarily hold more than
 *    max_items entries.  The physical slot capacity (2× max_items)
 *    provides the true upper bound; probe exhaustion is the only hard
 *    failure mode and signals a capacity-planning problem.
 *
 * 4. Set is an UPSERT.  Inserting an existing key replaces its value in
 *    place and does not change the item count.  Use LocklessLruSetEx() to
 *    learn whether the call inserted, replaced, or evicted, and to receive
 *    the displaced (now caller-owned) pointer.
 *
 * 5. Ownership: exactly like LockingLru, this structure never allocates or
 *    frees LruClientData payloads.  Set() may return an evicted or replaced
 *    item — that's now yours to free.  Remove() returns the removed item
 *    the same way.  A Get() result is valid only until the caller frees the
 *    item or a concurrent Remove/eviction reclaims the slot — synchronise
 *    at the application level if you need to extend a Get() result's
 *    lifetime past the next mutation.  LocklessLruGetRef() additionally
 *    returns a generation you can pass to LocklessLruRefStillValid() to
 *    detect slot reuse; it does not pin the payload.
 *
 * 6. Destroy is single-threaded with respect to in-flight operations —
 *    never call it while another thread is using the cache.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_H
#define UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>
#include <uflib/cdt/lockless_lru/cdt_lockless_lru_type.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/logger/logger_type.h>

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Create a new lockless LRU cache.
 *
 * All memory is allocated upfront — there are zero dynamic allocations
 * during the cache's operational lifetime.  The cache is bounded; once
 * the physical slot table is full, insertions fail gracefully.
 *
 * @param config_ptr  Immutable config (NULL → defaults from
 *                    cdt_lockless_lru_defs.h).
 * @return Opaque handle, or NULL on allocation failure or capacity overflow.
 *
 * @code{.c}
 * LocklessLruConfig cfg = { .capacity_hint = 1024 };
 * LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
 * if (!lru_ptr) { // handle allocation failure }
 * LocklessLruDestroy(lru_ptr);
 * @endcode
 */
PUBLIC_API LocklessLru *
LocklessLruCreate(const LocklessLruConfig *config_ptr);

/**
 * @brief Create an LRU cache that reports through @p logger_ptr.
 *
 * Behaves exactly as LocklessLruCreate(), and additionally reports creation,
 * each capacity/allocation failure, and release through the supplied logger.
 * Get, Set and Remove are never logged, so a cache built this way has the same
 * concurrency behaviour — and the same cache-line behaviour — as one built
 * without a logger.
 *
 * The failures are worth distinguishing: this constructor has five distinct
 * ways to return NULL and no other channel through which to say which one
 * happened.
 *
 * The logger is **borrowed, not owned**, and is deliberately not a field of
 * @ref LocklessLruConfig: the config describes what the cache should do, while
 * the logger is a collaborator the caller supplies.  Keeping them apart means a
 * config can be reused across caches without dragging a logger with it.
 *
 * The cache stores the pointer and never destroys it, so the logger must
 * outlive the cache: destroy the cache before destroying the logger.
 *
 * @param config_ptr  Immutable config (NULL → defaults from
 *                    cdt_lockless_lru_defs.h).
 * @param logger_ptr  Logger to report through, or NULL to report nothing.
 *                    NULL gives exactly LocklessLruCreate().
 *
 * @return Opaque handle, or NULL on allocation failure or capacity overflow.
 *
 * @code{.c}
 * UfLogger *log_ptr = NULL;
 * if (UfLoggerCreateWithDefaults(&log_ptr) != UF_LOGGER_STATUS_OK) { return NULL; }
 *
 * LocklessLruConfig cfg = { .capacity_hint = 1024 };
 * LocklessLru *lru_ptr = LocklessLruCreateWithLogger(&cfg, log_ptr);
 * if (!lru_ptr) { UfLoggerDestroy(log_ptr); return NULL; }
 *
 * LocklessLruDestroy(lru_ptr);   // the cache first —
 * UfLoggerDestroy(log_ptr);      // then the logger it borrowed
 * @endcode
 */
PUBLIC_API LocklessLru *
LocklessLruCreateWithLogger(const LocklessLruConfig *config_ptr, UfLogger *logger_ptr);

/**
 * @brief Tear down the cache and free all internal storage.
 *
 * Does NOT free LruClientData payloads — those are caller-owned.
 * NULL is safe (no-op).  Must only be called when no other threads
 * are accessing the cache.
 *
 * @param lru_ptr  Cache handle (NULL → no-op).
 */
PUBLIC_API void
LocklessLruDestroy(LocklessLru *lru_ptr);

/* ── Core operations ───────────────────────────────────────────────────── */

/**
 * @brief Insert or replace a key (upsert), returning the displaced pointer.
 *
 * Thin wrapper over LocklessLruSetEx(): returns only the displaced
 * pointer (evicted victim or replaced value) and NULL otherwise.  Use
 * LocklessLruSetEx() when you need to distinguish "inserted" from
 * "replaced" from "evicted" from "table full".
 *
 * @param lru_ptr   Cache handle.
 * @param key       Key to insert (uint64_t — hash composite keys yourself).
 * @param data_ptr  Opaque caller-owned payload.
 * @return Evicted or replaced LruClientData* (caller frees), or NULL.
 *
 * @code{.c}
 * LruClientData *displaced = LocklessLruSet(lru, key, new_value);
 * if (displaced) { free(displaced); } // evicted victim or replaced value
 * @endcode
 */
PUBLIC_API LruClientData *
LocklessLruSet(LocklessLru *lru_ptr, uint64_t key, LruClientData *data_ptr);

/**
 * @brief Insert or replace a key, reporting the exact outcome.
 *
 * Upsert semantics: an existing key is replaced in place (count
 * unchanged); a new key is inserted and, if the target capacity is now
 * exceeded, a CLOCK eviction runs after the insert (insert-then-trim).
 * Eviction is bounded and cannot livelock.
 *
 * @param lru_ptr   Cache handle.
 * @param key       Key to insert.
 * @param data_ptr  Opaque caller-owned payload.
 * @return LocklessLruSetResult with a status and the displaced pointer
 *         (owned by the caller — free it) on REPLACED/EVICTED.
 *
 * @code{.c}
 * LocklessLruSetResult r = LocklessLruSetEx(lru, key, value);
 * switch (r.status) {
 * case LOCKLESS_LRU_SET_REPLACED: free(r.displaced); break;
 * case LOCKLESS_LRU_SET_EVICTED:  free(r.displaced); break;
 * case LOCKLESS_LRU_SET_FULL:     break; // capacity-planning signal
 * case LOCKLESS_LRU_SET_INSERTED: break;
 * }
 * @endcode
 */
PUBLIC_API LocklessLruSetResult
LocklessLruSetEx(LocklessLru *lru_ptr, uint64_t key, LruClientData *data_ptr);

/**
 * @brief Look up a key and mark it recently-used.
 *
 * Lock-free: no CAS, no spinning on contention.  Uses seqlock-validated
 * optimistic reads — bounded number of steps, never blocks.
 *
 * @param lru_ptr  Cache handle.
 * @param key      Key to look up.
 * @return LruClientData* if found, or NULL if absent.
 */
PUBLIC_API LruClientData *
LocklessLruGet(LocklessLru *lru_ptr, uint64_t key);

/**
 * @brief Look up a key and return a generation-stamped snapshot.
 *
 * Like LocklessLruGet(), but also returns the slot generation so the
 * caller can later detect reuse via LocklessLruRefStillValid().  The
 * returned @p data is still only a borrowed pointer — @p gen does not pin
 * the payload.
 *
 * @param lru_ptr  Cache handle.
 * @param key      Key to look up.
 * @return LocklessLruRef with .data = payload (or NULL) and .gen = generation.
 *
 * @code{.c}
 * LocklessLruRef ref = LocklessLruGetRef(lru, key);
 * if (ref.data && LocklessLruRefStillValid(lru, key, ref.gen)) {
 *     // slot not reused since the snapshot
 * }
 * @endcode
 */
PUBLIC_API LocklessLruRef
LocklessLruGetRef(LocklessLru *lru_ptr, uint64_t key);

/**
 * @brief Test whether a generation snapshot is still current for a key.
 *
 * @param lru_ptr  Cache handle.
 * @param key      Key whose slot is in question.
 * @param gen      Generation returned by an earlier LocklessLruGetRef().
 * @return true if the key is present and its slot generation still matches
 *         @p gen (no reuse since the snapshot); false otherwise.
 */
PUBLIC_API bool
LocklessLruRefStillValid(LocklessLru *lru_ptr, uint64_t key, uint64_t gen);

/**
 * @brief Remove a key if present.
 *
 * @param lru_ptr  Cache handle.
 * @param key      Key to remove.
 * @return Removed LruClientData* (caller frees), or NULL if absent.
 */
PUBLIC_API LruClientData *
LocklessLruRemove(LocklessLru *lru_ptr, uint64_t key);

/**
 * @brief Current item count (acquire-ordered snapshot).
 *
 * @param lru_ptr  Cache handle.
 * @return Approximate count — under concurrent mutation the snapshot
 *         may be slightly stale by the time the caller reads it.
 */
PUBLIC_API size_t
LocklessLruSize(LocklessLru *lru_ptr);

/**
 * @brief Physical slot capacity of the table.
 *
 * The hard ceiling on the number of simultaneously occupied slots
 * (2× capacity_hint, rounded up to a power of two, minimum 8).
 *
 * @param lru_ptr  Cache handle.
 * @return Physical slot count, or 0 for a NULL handle.
 */
PUBLIC_API size_t
LocklessLruCapacity(LocklessLru *lru_ptr);

/* ── Introspection ─────────────────────────────────────────────────────── */

/**
 * @brief Describe the cache's state as a JSON string.
 *
 * Composes a best-effort introspection snapshot: physical capacity, logical
 * target (`max_items`), current item count, CLOCK hand position, load factor,
 * whether this cache was created with a logger to report through (`logger`,
 * `"enabled"` or `"none"`), an occupancy breakdown (`occupied` / `empty` /
 * `tombstone` / `referenced`), and one entry per occupied slot (`index`, `key`,
 * `referenced`, `gen`).
 *
 * All reads are relaxed snapshots, so figures may be slightly stale under
 * concurrent mutation — this is introspection, not a transactional view.  The
 * atomic `count` and the walked `occupied` total are both emitted so a reader
 * can spot a persistent drift between them.
 *
 * A NULL handle emits `{"error":"null handle"}` and carries no `logger`
 * attribute: with no instance there is no borrow to report.
 *
 * If @p provided is NULL, a fresh BufferDescriptor is allocated (the caller
 * must free it with BufferDescriptorRelease() + free()); otherwise the caller's
 * descriptor is appended to.  Returns @p provided, or NULL only if a fresh
 * descriptor could not be allocated.
 *
 * @param lru_ptr   Cache handle (NULL → emits `{"error":"null handle"}`).
 * @param provided  Optional caller-owned BufferDescriptor (NULL → allocate).
 * @return The populated BufferDescriptor.
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 512);
 * DescribeLocklessLru(lru_ptr, &bd);
 * printf("%s\n", bd.data);
 * BufferDescriptorRelease(&bd);
 * @endcode
 */
PUBLIC_API BufferDescriptor *
DescribeLocklessLru(LocklessLru *lru_ptr, BufferDescriptor *provided);

#endif /* UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_H */
