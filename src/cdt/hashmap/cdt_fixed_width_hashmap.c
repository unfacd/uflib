/**
 * @file cdt_fixed_width_hashmap.c
 * @brief Lockless fixed-width open-addressing hash map — implementation.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/cdt/hashmap/cdt_fixed_width_hashmap.h>

#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ───────────────────────────────────────────────────── */

/*! Compute the per-node allocation size (header + key_width). */
static inline size_t
sNodeSize(uint32_t key_width)
{
    /* Round up to cache-line alignment (64) to minimise false sharing. */
    size_t raw = sizeof(CDSHashMapNode) + key_width;
    return (raw + 63U) & ~((size_t)63U);
}

/* ── FNV-1a hash (on truncated key) ─────────────────────────────────────── */

/*!
 * Compute the FNV-1a hash of a key, truncated to at most @p key_width bytes.
 * This ensures Insert and Get/Remove always hash to the same bucket even when
 * the caller passes a key longer than the map's key width.
 */
static uint32_t
sHashTruncated(const char *str, uint32_t key_width)
{
    uint32_t hash = 2166136261U;
    uint32_t i    = 0;
    while (*str && i < key_width - 1) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619U;
        i++;
    }
    return hash;
}

/* ── Power-of-2 helpers ─────────────────────────────────────────────────── */

static uint32_t
sNextPow2(uint32_t v)
{
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

/* ── Treiber stack (free list) with ABA-protection ──────────────────────── */

/* Pack/unpack a 64-bit ABA-protected stack pointer:
 *   upper 32 bits = pop counter
 *   lower 32 bits = pool index
 */
static inline uintptr_t
sPackFreeNode(uint32_t idx, uint64_t pop_count)
{
    return ((uintptr_t)pop_count << 32) | idx;
}

static inline uint32_t
sUnpackFreeIdx(uintptr_t val)
{
    return (uint32_t)(val & 0xFFFFFFFFU);
}

static inline uint64_t
sUnpackPopCount(uintptr_t val)
{
    return (uint64_t)(val >> 32);
}

/*!
 * Pop a free node index from the Treiber stack.
 * Returns 0 if the pool is exhausted (index 0 is reserved — never allocated).
 */
static uint32_t
sPoolAllocate(LocklessFixedWidthHashMap *map)
{
    uintptr_t head =
        atomic_load_explicit(&map->free_stack_head, memory_order_acquire);

    while (head != 0) {
        uint32_t idx = sUnpackFreeIdx(head);
        CDSHashMapNode *node =
            (CDSHashMapNode *)((char *)map->pool + (size_t)idx * sNodeSize(map->key_width));

        uint32_t next_free =
            atomic_load_explicit(&node->next_free, memory_order_relaxed);

        uintptr_t new_head = sPackFreeNode(
            next_free, sUnpackPopCount(head) + 1);

        if (atomic_compare_exchange_strong_explicit(
                &map->free_stack_head, &head, new_head,
                memory_order_acq_rel, memory_order_relaxed)) {
            /* Seq-lock Phase 1: bump generation immediately upon allocation.
             * This makes gen odd BEFORE we write the payload, so any stale
             * reader holding this pool index from a previous lifecycle will
             * see an odd gen and retry immediately — they never see a
             * consistent gen with torn payload data. */
            atomic_fetch_add_explicit(&node->generation, 1ULL,
                                      memory_order_release);
            return idx;
        }
    }
    return 0; /* Pool exhausted */
}

/*!
 * Push a pool index back onto the Treiber stack.
 */
static void
sPoolFree(LocklessFixedWidthHashMap *map, uint32_t idx)
{
    uintptr_t old_head =
        atomic_load_explicit(&map->free_stack_head, memory_order_acquire);
    uintptr_t new_head;
    do {
        CDSHashMapNode *node =
            (CDSHashMapNode *)((char *)map->pool + (size_t)idx * sNodeSize(map->key_width));
        atomic_store_explicit(&node->next_free, sUnpackFreeIdx(old_head),
                              memory_order_relaxed);
        new_head = sPackFreeNode(idx, sUnpackPopCount(old_head) + 1);
    } while (!atomic_compare_exchange_strong_explicit(
                 &map->free_stack_head, &old_head, new_head,
                 memory_order_acq_rel, memory_order_relaxed));
}

/* ── Node helpers ───────────────────────────────────────────────────────── */

/* Return a pointer to a pool node by index. */
static inline CDSHashMapNode *
sNodeAt(LocklessFixedWidthHashMap *map, uint32_t idx)
{
    return (CDSHashMapNode *)((char *)map->pool +
                              (size_t)idx * sNodeSize(map->key_width));
}

/*!
 * Seq-lock validated key comparison.
 *
 * Reads the generation counter before and after reading key/value from the
 * pool node.  If the generation changed (or is odd — write in progress),
 * the read was inconsistent and the caller must retry.
 *
 * @param map      The map (needed for key_width).
 * @param node     Pool node to read from.
 * @param key      Search key.
 * @param out_val  Receives the node's value if the keys match.
 * @param out_gen  Receives the generation at which the read was valid.
 * @return true if the keys match and the read was consistent.
 */
static bool
sNodeKeyMatch(LocklessFixedWidthHashMap *map, CDSHashMapNode *node,
              const char *key, uint32_t *out_val, uint64_t *out_gen)
{
    uint64_t gen_before, gen_after;

    /* Read generation with acquire — synchronises with writer's release. */
    gen_before = atomic_load_explicit(&node->generation, memory_order_acquire);

    /* If odd, a write is in progress — retry immediately. */
    if (gen_before & 1ULL) {
        return false;
    }

    /* Read key and value.  The acquire on generation ensures these loads
     * see the writer's stores (on all memory models). */
    int cmp = strncmp(node->key, key, map->key_width);
    uint32_t val = node->value;

    /* Prevent compiler from reordering the strncmp/value reads around
     * the second generation load. */
    atomic_thread_fence(memory_order_acquire);

    gen_after = atomic_load_explicit(&node->generation, memory_order_relaxed);

    if (gen_before != gen_after) {
        /* Node was recycled during our read — inconsistent. */
        return false;
    }

    if (cmp != 0) {
        return false;
    }

    if (out_val) *out_val = val;
    if (out_gen) *out_gen = gen_after;
    return true;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

PUBLIC_API bool
LocklessFixedWidthHashMap_Init(LocklessFixedWidthHashMap *map,
                                uint32_t                   capacity,
                                uint32_t                   load_factor_pct,
                                uint32_t                   key_width)
{
    if (!map) return false;
    if (capacity == 0) return false;
    if (load_factor_pct == 0 || load_factor_pct > 100) return false;
    if (key_width == 0 ||
        key_width > CONFIG_DEFAULT_CDS_HASHMAP_KEY_WIDTH) return false;

    memset(map, 0, sizeof(*map));

    map->slot_capacity = sNextPow2(capacity);
    map->slot_mask     = map->slot_capacity - 1;
    map->key_width     = key_width;

    /* Pool size = slot_capacity × load_factor_pct / 100.
     * Add 1 to account for index 0, which is reserved as the EMPTY
     * sentinel and never allocated from the free stack. */
    uint64_t pool_sz =
        ((uint64_t)map->slot_capacity * load_factor_pct) / 100U;
    if (pool_sz == 0) pool_sz = 1;
    pool_sz += 1;  /* +1 for reserved index 0 */
    map->pool_capacity = (uint32_t)pool_sz;

    /* Allocate slots (zero = EMPTY) */
    map->slots = calloc(map->slot_capacity, sizeof(*map->slots));
    if (!map->slots) {
        return false;
    }

    /* Allocate pool as a contiguous byte array (FAM nodes have varying sizes) */
    size_t node_sz = sNodeSize(key_width);
    map->pool = calloc(map->pool_capacity, node_sz);
    if (!map->pool) {
        free((void *)map->slots);
        map->slots = NULL;
        return false;
    }

    /* Seed the free stack with all pool indices in reverse order.
     * Index 0 is reserved (EMPTY sentinel) — never allocated. */
    atomic_init(&map->free_stack_head, 0);
    for (uint32_t i = map->pool_capacity - 1; i > 0; i--) {
        CDSHashMapNode *node = sNodeAt(map, i);
        atomic_init(&node->generation, 0);
        atomic_init(&node->next_free, 0);
        sPoolFree(map, i);
    }

    atomic_init(&map->size, 0);
    return true;
}

PUBLIC_API void
LocklessFixedWidthHashMap_Destroy(LocklessFixedWidthHashMap *map)
{
    if (!map) return;

    if (map->slots) {
        free((void *)map->slots);
        map->slots = NULL;
    }
    if (map->pool) {
        free(map->pool);
        map->pool = NULL;
    }
    map->slot_capacity = 0;
    map->slot_mask     = 0;
    map->pool_capacity = 0;
    map->key_width     = 0;
    atomic_store_explicit(&map->free_stack_head, 0, memory_order_relaxed);
    atomic_store_explicit(&map->size, 0, memory_order_relaxed);
}

/* ── Core operations ───────────────────────────────────────────────────── */

PUBLIC_API bool
LocklessFixedWidthHashMap_Insert(LocklessFixedWidthHashMap *map,
                                  const char                *key,
                                  uint32_t                   value,
                                  bool                      *out_is_new)
{
    if (!map || !key) return false;

    uint32_t  hash      = sHashTruncated(key, map->key_width);
    uint32_t  start_idx = hash & map->slot_mask;
    uint32_t  max_probe = PRIV_CONFIG_DEFAULT_CDS_HASHMAP_MAX_PROBE;

    if (max_probe > map->slot_capacity) {
        max_probe = map->slot_capacity;
    }

    for (uint32_t i = 0; i < max_probe; i++) {
        uint32_t idx = (start_idx + i) & map->slot_mask;
        if (idx == 0) continue; /* Slot 0 is reserved */

        uint32_t slot_val =
            atomic_load_explicit(&map->slots[idx], memory_order_acquire);

        /* ── Empty or tombstone — try to claim this slot ── */
        if (slot_val == CDS_HASHMAP_SLOT_EMPTY ||
            slot_val == CDS_HASHMAP_SLOT_TOMBSTONE) {

            /* Allocate a new node from the free list. */
            uint32_t new_idx = sPoolAllocate(map);
            if (new_idx == 0) return false; /* Pool exhausted */

            CDSHashMapNode *new_node = sNodeAt(map, new_idx);

            /* Write key and value.  sPoolAllocate() already bumped
             * generation from even→odd (write-locked), so any stale
             * reader seeing this node will encounter odd gen and retry.
             * The non-atomic writes below are therefore safe — no reader
             * will inspect the payload until we publish via the CAS and
             * bump gen back to even. */
            strncpy(new_node->key, key, map->key_width);
            new_node->key[map->key_width - 1] = '\0';
            new_node->value = value;

            /* Fence: all non-atomic stores above must be visible before
             * the slot CAS publishes the node to other threads. */
            atomic_thread_fence(memory_order_release);

            /* Try to publish the node into the slot. */
            if (atomic_compare_exchange_strong_explicit(
                    &map->slots[idx], &slot_val, new_idx,
                    memory_order_acq_rel, memory_order_relaxed)) {
                /* Success!  Bump generation: odd → even (write done).
                 * sPoolAllocate set gen to odd; this makes it even,
                 * signalling readers that the payload is stable. */
                atomic_fetch_add_explicit(&new_node->generation, 1ULL,
                                          memory_order_release);
                atomic_fetch_add_explicit(&map->size, 1, memory_order_relaxed);
                if (out_is_new) *out_is_new = true;
                return true;
            }

            /* CAS failed — another thread claimed the slot first.
             * Return the node to the free list.  Bump generation
             * (odd→even) to mark this allocation attempt as dead. */
            atomic_fetch_add_explicit(&new_node->generation, 1ULL,
                                      memory_order_release);
            sPoolFree(map, new_idx);

            /* The CAS failure updated slot_val to the current slot content.
             * It could now be a valid entry (the other thread's insert),
             * or still EMPTY/TOMBSTONE (the other thread inserted elsewhere).
             * Fall through to re-check. */
            if (slot_val == CDS_HASHMAP_SLOT_EMPTY ||
                slot_val == CDS_HASHMAP_SLOT_TOMBSTONE) {
                continue; /* Retry same slot */
            }
            /* slot_val is now a valid pool index — check if it's our key */
        }

        /* ── Occupied slot — check for matching key ── */
        if (slot_val != CDS_HASHMAP_SLOT_TOMBSTONE && slot_val != 0) {
            CDSHashMapNode *existing = sNodeAt(map, slot_val);
            uint64_t gen;
            uint32_t existing_val;

            if (sNodeKeyMatch(map, existing, key, &existing_val, &gen)) {
                /* Key already exists. */
                if (out_is_new) *out_is_new = false;
                (void)gen; /* Generation captured for caller debugging */
                return true;
            }
        }
    }

    return false; /* Max probe exceeded — map is pathologically full */
}

PUBLIC_API bool
LocklessFixedWidthHashMap_Get(LocklessFixedWidthHashMap *map,
                               const char                *key,
                               uint32_t                  *out_value)
{
    if (!map || !key) return false;

    uint32_t hash      = sHashTruncated(key, map->key_width);
    uint32_t start_idx = hash & map->slot_mask;
    uint32_t max_probe = PRIV_CONFIG_DEFAULT_CDS_HASHMAP_MAX_PROBE;

    if (max_probe > map->slot_capacity) {
        max_probe = map->slot_capacity;
    }

    for (uint32_t i = 0; i < max_probe; i++) {
        uint32_t idx = (start_idx + i) & map->slot_mask;
        if (idx == 0) continue;

        uint32_t slot_val =
            atomic_load_explicit(&map->slots[idx], memory_order_acquire);

        if (slot_val == CDS_HASHMAP_SLOT_EMPTY) {
            /* End of probe chain — key not present. */
            return false;
        }

        if (slot_val != CDS_HASHMAP_SLOT_TOMBSTONE && slot_val != 0) {
            CDSHashMapNode *node = sNodeAt(map, slot_val);
            uint64_t gen;
            uint32_t val;

            if (sNodeKeyMatch(map, node, key, &val, &gen)) {
                if (out_value) *out_value = val;
                (void)gen;
                return true;
            }
        }
    }

    return false;
}

PUBLIC_API bool
LocklessFixedWidthHashMap_Remove(LocklessFixedWidthHashMap *map,
                                  const char                *key,
                                  uint32_t                  *out_value)
{
    if (!map || !key) return false;

    uint32_t hash      = sHashTruncated(key, map->key_width);
    uint32_t start_idx = hash & map->slot_mask;
    uint32_t max_probe = PRIV_CONFIG_DEFAULT_CDS_HASHMAP_MAX_PROBE;

    if (max_probe > map->slot_capacity) {
        max_probe = map->slot_capacity;
    }

    for (uint32_t i = 0; i < max_probe; i++) {
        uint32_t idx = (start_idx + i) & map->slot_mask;
        if (idx == 0) continue;

        uint32_t slot_val =
            atomic_load_explicit(&map->slots[idx], memory_order_acquire);

        if (slot_val == CDS_HASHMAP_SLOT_EMPTY) {
            return false; /* End of probe chain */
        }

        if (slot_val != CDS_HASHMAP_SLOT_TOMBSTONE && slot_val != 0) {
            CDSHashMapNode *node = sNodeAt(map, slot_val);
            uint64_t gen;
            uint32_t val;

            if (sNodeKeyMatch(map, node, key, &val, &gen)) {
                /* Capture value before we destroy the entry. */
                if (out_value) *out_value = val;

                /* Pre-CAS validation: atomically bump the generation from
                 * the value we validated in sNodeKeyMatch to gen+1 (odd).
                 * If the CAS fails, the generation changed — the node was
                 * recycled between our key-match read and now.  Abort. */
                uint64_t expected_gen = gen;
                if (!atomic_compare_exchange_strong_explicit(
                        &node->generation, &expected_gen, expected_gen + 1ULL,
                        memory_order_acq_rel, memory_order_relaxed)) {
                    /* Node was recycled — our snapshot is stale. */
                    return false;
                }

                /* Try to atomically replace the slot with TOMBSTONE. */
                if (atomic_compare_exchange_strong_explicit(
                        &map->slots[idx], &slot_val,
                        CDS_HASHMAP_SLOT_TOMBSTONE,
                        memory_order_acq_rel, memory_order_relaxed)) {
                    /* Bump generation again: odd → even (node is dead).
                     * Use fetch_add here because we already own the node
                     * (the CAS on gen above succeeded). */
                    atomic_fetch_add_explicit(&node->generation, 1ULL,
                                              memory_order_release);
                    /* Clear the key to prevent false matches if re-allocated
                     * before the generation bump is observed. */
                    memset(node->key, 0, map->key_width);
                    sPoolFree(map, slot_val);
                    atomic_fetch_sub_explicit(&map->size, 1,
                                              memory_order_relaxed);
                    return true;
                }

                /* Slot CAS failed — another thread removed (or replaced)
                 * the slot first.  Roll back the generation bump so the
                 * node is still usable (return to even state). */
                atomic_fetch_sub_explicit(&node->generation, 1ULL,
                                          memory_order_release);
                return false;
            }
        }
    }

    return false;
}

/* ── Query ──────────────────────────────────────────────────────────────── */

PUBLIC_API uint32_t
LocklessFixedWidthHashMap_Size(const LocklessFixedWidthHashMap *map)
{
    if (!map) return 0;
    return atomic_load_explicit(
        (const _Atomic uint32_t *)&map->size, memory_order_relaxed);
}

PUBLIC_API uint32_t
LocklessFixedWidthHashMap_Capacity(const LocklessFixedWidthHashMap *map)
{
    if (!map) return 0;
    return map->slot_capacity;
}

PUBLIC_API float
LocklessFixedWidthHashMap_LoadFactor(const LocklessFixedWidthHashMap *map)
{
    if (!map || map->slot_capacity == 0) return 0.0f;
    uint32_t sz = LocklessFixedWidthHashMap_Size(map);
    return (float)sz / (float)map->slot_capacity;
}
