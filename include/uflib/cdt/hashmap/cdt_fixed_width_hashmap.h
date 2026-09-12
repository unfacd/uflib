/**
 * @file cdt_fixed_width_hashmap.h
 * @brief Lockless fixed-width open-addressing hash map — public API.
 *
 * LocklessFixedWidthHashMap is a bounded, lock-free, open-addressing hash map
 * that maps fixed-width C-string keys to uint32_t values.  All memory is
 * pre-allocated at init time — there are zero malloc/free calls during
 * operational use.
 *
 * ## Thread safety
 *
 * Every operation is lock-free and safe to call concurrently from any number
 * of threads.  Destroy() is the sole exception: it must only be called when
 * no other threads are accessing the map.
 *
 * ## Read-side safety (seq-lock)
 *
 * Readers validate each pool-node access with a generation-counter seq-lock:
 * they read the generation before and after accessing the node data, and retry
 * if the generation changed (indicating the node was recycled mid-read).
 * This eliminates the stale-pointer problem without requiring per-thread
 * hazard-pointer arrays.
 *
 * ## Memory ordering
 *
 * All slot loads use acquire ordering; all slot stores/CAS use release or
 * acq_rel.  A release fence between pool-node writes and the slot CAS ensures
 * correctness on weakly-ordered architectures (ARM, PowerPC).
 *
 * ## Capacity
 *
 * The slot table is sized to the next power of 2 ≥ the requested capacity.
 * The node pool is sized to (slot_capacity × load_factor / 100), leaving
 * headroom so linear probing stays efficient.  Once the pool is exhausted,
 * insertions fail gracefully.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_HASHMAP_CDT_FIXED_WIDTH_HASHMAP_H
#define UFLIB_CDT_HASHMAP_CDT_FIXED_WIDTH_HASHMAP_H

#include <stdbool.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>
#include <uflib/cdt/hashmap/lockless_fixed_width_hashmap_type.h>
#include <uflib/cdt/hashmap/cdt_fixed_width_hashmap_defs.h>

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/**
 * @brief Initialise a LocklessFixedWidthHashMap.
 *
 * All memory (slot table + node pool) is allocated upfront.  The actual slot
 * count is rounded up to the next power of 2.  The pool is sized to
 * (slot_capacity × load_factor_pct / 100).
 *
 * @param map              Pointer to an uninitialised map struct.
 * @param capacity         Requested minimum capacity (rounded up to power of 2).
 * @param load_factor_pct  Pool size as percentage of slot count (1–100).
 * @param key_width        Key width in bytes (≤ CONFIG_DEFAULT_CDS_HASHMAP_KEY_WIDTH).
 * @return true on success, false on invalid parameters or allocation failure.
 *
 * @code{.c}
 * LocklessFixedWidthHashMap map;
 * if (!LocklessFixedWidthHashMap_Init(&map, 1024, 75,
 *                                     CONFIG_DEFAULT_CDS_HASHMAP_KEY_WIDTH)) {
 *     fprintf(stderr, "init failed\n");
 *     return EXIT_FAILURE;
 * }
 * @endcode
 */
PUBLIC_API bool LocklessFixedWidthHashMap_Init(
    LocklessFixedWidthHashMap *map,
    uint32_t                   capacity,
    uint32_t                   load_factor_pct,
    uint32_t                   key_width);

/**
 * @brief Destroy the map and free all allocated memory.
 *
 * @warning No other threads may be accessing the map when this is called.
 *
 * @param map  Map to destroy (idempotent — safe to call on an empty map).
 */
PUBLIC_API void LocklessFixedWidthHashMap_Destroy(LocklessFixedWidthHashMap *map);

/* ── Core operations ───────────────────────────────────────────────────── */

/**
 * @brief Insert a key-value pair, or retrieve an existing key's value.
 *
 * If @p key does not exist, it is associated with @p value and @p out_is_new
 * is set to true.  If @p key already exists, the existing value is returned
 * unchanged and @p out_is_new is set to false.  This check-then-act is a
 * single linearizable atomic operation — two threads racing to insert the
 * same key will never create duplicates.
 *
 * @param map          Initialised map.
 * @param key          NUL-terminated key string (must fit in key_width bytes).
 * @param value        Value to store if the key is new.
 * @param out_is_new   Set to true if inserted, false if key already existed.
 *                     May be NULL if the caller doesn't care.
 * @return true on success, false if the pool is exhausted.
 *
 * @code{.c}
 * bool is_new = false;
 * if (!LocklessFixedWidthHashMap_Insert(&map, "foo", 42, &is_new)) {
 *     fprintf(stderr, "map full\n");
 *     return;
 * }
 * if (is_new) {
 *     printf("Inserted new entry\n");
 * } else {
 *     printf("Key already existed, value unchanged\n");
 * }
 * @endcode
 */
PUBLIC_API bool LocklessFixedWidthHashMap_Insert(
    LocklessFixedWidthHashMap *map,
    const char                *key,
    uint32_t                   value,
    bool                      *out_is_new);

/**
 * @brief Retrieve the value associated with a key.
 *
 * @param map        Initialised map.
 * @param key        NUL-terminated key string.
 * @param out_value  Receives the associated value.  May be NULL.
 * @return true if the key was found and @p out_value was populated.
 *
 * @code{.c}
 * uint32_t val = 0;
 * if (LocklessFixedWidthHashMap_Get(&map, "foo", &val)) {
 *     printf("foo → %u\n", val);
 * }
 * @endcode
 */
PUBLIC_API bool LocklessFixedWidthHashMap_Get(
    LocklessFixedWidthHashMap *map,
    const char                *key,
    uint32_t                  *out_value);

/**
 * @brief Remove a key and its associated value from the map.
 *
 * @param map        Initialised map.
 * @param key        NUL-terminated key string.
 * @param out_value  Receives the value that was associated with the key.
 *                   May be NULL if the caller doesn't need it.
 * @return true if the key was found and removed.
 *
 * @code{.c}
 * uint32_t old_val = 0;
 * if (LocklessFixedWidthHashMap_Remove(&map, "foo", &old_val)) {
 *     printf("Removed foo (was %u)\n", old_val);
 * }
 * @endcode
 */
PUBLIC_API bool LocklessFixedWidthHashMap_Remove(
    LocklessFixedWidthHashMap *map,
    const char                *key,
    uint32_t                  *out_value);

/* ── Query ──────────────────────────────────────────────────────────────── */

/**
 * @brief Approximate number of entries currently in the map.
 *
 * The count is approximate because it is updated outside any global lock.
 * It will never overcount (a key is never counted twice) but may briefly
 * undercount during concurrent insert/remove operations.
 *
 * @param map  Initialised map.
 * @return Approximate entry count.
 */
PUBLIC_API uint32_t LocklessFixedWidthHashMap_Size(
    const LocklessFixedWidthHashMap *map);

/**
 * @brief Slot capacity (the power-of-2 table size).
 *
 * @param map  Initialised map.
 * @return Number of hash slots.
 */
PUBLIC_API uint32_t LocklessFixedWidthHashMap_Capacity(
    const LocklessFixedWidthHashMap *map);

/**
 * @brief Current load factor as a fraction (0.0 – 1.0).
 *
 * @param map  Initialised map.
 * @return size / slot_capacity, or 0.0 for an empty map.
 */
PUBLIC_API float LocklessFixedWidthHashMap_LoadFactor(
    const LocklessFixedWidthHashMap *map);

#endif /* UFLIB_CDT_HASHMAP_CDT_FIXED_WIDTH_HASHMAP_H */
