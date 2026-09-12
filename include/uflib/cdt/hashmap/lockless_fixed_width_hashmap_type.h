/**
 * @file lockless_fixed_width_hashmap_type.h
 * @brief Lockless fixed-width open-addressing hash map — type definitions.
 *
 * This header contains the struct definition and embedded constants for the
 * LocklessFixedWidthHashMap.  Consumers that only need the type (e.g. for
 * opaque embedding in other structs) can include just this file without
 * pulling in the full API surface.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_HASHMAP_LOCKLESS_FIXED_WIDTH_HASHMAP_TYPE_H
#define UFLIB_CDT_HASHMAP_LOCKLESS_FIXED_WIDTH_HASHMAP_TYPE_H

#include <stdatomic.h>
#include <stdint.h>

/* ── Slot sentinel values ───────────────────────────────────────────────── */

#define CDS_HASHMAP_SLOT_EMPTY     0x0U         ///< Slot has never been occupied
#define CDS_HASHMAP_SLOT_TOMBSTONE 0xFFFFFFFFU  ///< Slot was occupied but entry was deleted

/* ── Pool node ──────────────────────────────────────────────────────────── */

/*!
 * A single key-value entry in the pre-allocated node pool.
 *
 * The @p generation field is used as a sequence-lock version counter:
 * every time the node is allocated or freed the generation is incremented,
 * allowing readers to detect recycling without hazard pointers.
 */
typedef struct CDSHashMapNode {
    _Atomic uint64_t generation;   ///< Seq-lock version (odd = write in progress)
    _Atomic uint32_t next_free;    ///< Treiber-stack link when in free list
    /* ── key is a FAM whose actual size is set at map-init time ── */
    uint32_t        value;        ///< Opaque user value
    char            key[];        ///< Flexible array member — sized per-map
} CDSHashMapNode;

/* ── Map ─────────────────────────────────────────────────────────────────── */

/*!
 * Lock-free, fixed-width, open-addressing hash map with pre-allocated storage.
 *
 * All memory is allocated upfront during Init().  There are zero dynamic
 * allocations during the map's operational lifetime.  The map is bounded —
 * once full, insertions fail gracefully rather than growing.
 *
 * Thread safety: all operations are lock-free and safe to call concurrently
 * from multiple threads.  Destroy() must only be called when no other threads
 * are accessing the map.
 */
typedef struct LocklessFixedWidthHashMap {
    CDSHashMapNode   *pool;              ///< Pre-allocated node array (FAM-sized)
    _Atomic uint32_t *slots;             ///< Hash table — indices into pool
    uint32_t          slot_capacity;     ///< Number of slots (power of 2)
    uint32_t          slot_mask;         ///< slot_capacity - 1
    uint32_t          pool_capacity;     ///< Number of nodes in pool
    uint32_t          key_width;         ///< Bytes per key (from config or init)
    _Atomic uintptr_t free_stack_head;   ///< Treiber stack of free pool indices
    _Atomic uint32_t  size;              ///< Approximate entry count
} LocklessFixedWidthHashMap;

#endif /* UFLIB_CDT_HASHMAP_LOCKLESS_FIXED_WIDTH_HASHMAP_TYPE_H */
