/**
 * @file cdt_lockless_lru_type.h
 * @brief Lockless approximate LRU cache — type definitions.
 *
 * This header contains the configuration struct, the set-result and
 * reference types, and the opaque handle forward-declaration for the
 * LocklessLru cache.  Consumers that only need the types (e.g. for
 * embedding in another struct) can include just this file without
 * pulling in the full API surface.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_TYPE_H
#define UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_TYPE_H

#include <stddef.h>
#include <stdint.h>

/*!
 * Opaque caller-owned payload.
 *
 * The LRU never allocates, frees, or dereferences this pointer — it is
 * purely a handle that the consumer associates with a key.  The consumer
 * defines the concrete struct and casts to/from LruClientData * at the
 * call site.
 */
typedef struct LruClientData LruClientData;

/*!
 * Immutable configuration consumed by LocklessLruCreate().
 *
 * Set any field to 0 to accept the compile-time default (from
 * cdt_lockless_lru_defs.h).
 */
typedef struct LocklessLruConfig {
    size_t  capacity_hint;   ///< Target max items (0 → CONFIG_DEFAULT)
} LocklessLruConfig;

/*!
 * Opaque handle to a lockless approximate LRU cache instance.
 *
 * The struct definition is private (src/cdt/lockless_lru/cdt_lockless_lru_priv.h).
 * Consumers never allocate or inspect this struct directly — they obtain
 * it via LocklessLruCreate() and destroy it via LocklessLruDestroy().
 */
typedef struct LocklessLru LocklessLru;

/*!
 * Outcome of a LocklessLruSetEx() call.
 *
 * Set is an upsert: an existing key is replaced in place, a new key is
 * inserted, and eviction (if the target capacity is exceeded) runs after
 * the insert.  The three terminal conditions are now distinguishable —
 * the old NULL-overloaded return is gone.
 */
typedef enum LocklessLruSetStatus {
    LOCKLESS_LRU_SET_INSERTED = 0,  ///< New key inserted; no eviction occurred.
    LOCKLESS_LRU_SET_REPLACED,      ///< Existing key replaced; displaced = old value.
    LOCKLESS_LRU_SET_EVICTED,       ///< New key inserted; displaced = evicted victim.
    LOCKLESS_LRU_SET_FULL           ///< No insert — physical slot table exhausted.
} LocklessLruSetStatus;

/*!
 * Result of LocklessLruSetEx().
 *
 * @p displaced is the pointer the caller now owns and must free: the
 * evicted victim on EVICTED, the previous value on REPLACED, and NULL
 * otherwise.
 */
typedef struct LocklessLruSetResult {
    LocklessLruSetStatus status;     ///< What happened (see LocklessLruSetStatus).
    LruClientData       *displaced;  ///< Caller-owned pointer (free it), or NULL.
} LocklessLruSetResult;

/*!
 * A validated snapshot of a cached entry: the payload plus a generation.
 *
 * @p gen is bumped every time the slot is occupied, replaced, evicted, or
 * removed.  Pass it back to LocklessLruRefStillValid() to detect whether
 * the slot was reused since the snapshot.  It does NOT pin the payload —
 * the pointer can still be freed by a concurrent Remove/eviction; @p gen
 * only tells you the slot identity changed.
 */
typedef struct LocklessLruRef {
    LruClientData *data;   ///< Borrowed payload (valid only until reuse).
    uint64_t       gen;    ///< Slot generation at snapshot time.
} LocklessLruRef;

#endif /* UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_TYPE_H */
