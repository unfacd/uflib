/**
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

/**
 * @file recycler_v2_priv.h
 * @brief RecyclerV2 private types — exposed for test access only, NEVER installed
 *
 * This header is colocated with recycler_v2.c under src/.  It exposes internal
 * implementation types for unit-test inspection and implementation use.
 *
 * CONSUMERS MUST NEVER INCLUDE THIS HEADER.
 */

#ifndef UFLIB_RECYCLER_V2_RECYCLER_V2_PRIV_H
#define UFLIB_RECYCLER_V2_RECYCLER_V2_PRIV_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <uflib/recycler_v2/recycler_v2_type.h>
#include <uflib/recycler_v2/instance_holder_v2_type.h>
#include <uflib/recycler_v2/recycler_v2_defs.h>

/* ── Fallback multi-holder list ───────────────────────────────────────────── */

/*! Linked-list item for multi-InstanceHolderV2 per object (cold path). */
typedef struct ListItemInstanceV2 {
    InstanceHolderV2           *holder_ptr;
    struct ListItemInstanceV2  *next_ptr;
} ListItemInstanceV2;

/*! Linked-list wrapper node for pending unmarshal queue.
 *  InstanceHolderV2 is caller-allocated so we can't embed a next pointer
 *  directly — each enqueued holder gets a heap-allocated wrapper. */
typedef struct PendingUnmarshalNodeV2 {
    InstanceHolderV2              *holder_ptr;
    struct PendingUnmarshalNodeV2 *next_ptr;
} PendingUnmarshalNodeV2;

/*! Mutex-protected linked list of marshalled holders awaiting resolution.
 *  Cold path — only used by RecyclerV2GetInstanceTry (async) + Put drain. */
typedef struct PendingUnmarshalQueueV2 {
    PendingUnmarshalNodeV2 *head_ptr;
    PendingUnmarshalNodeV2 *tail_ptr;
    size_t                  count;
    pthread_mutex_t         mutex;          ///< protects all mutations
} PendingUnmarshalQueueV2;

/*! Fallback linked list — allocated only when RecyclerV2GetNewInstance()
 *  is called on an object that already has a holder. */
typedef struct InstancesListV2 {
    size_t               size;
    ListItemInstanceV2  *head_ptr;
    ListItemInstanceV2  *tail_ptr;
    pthread_spinlock_t   spin_lock;    ///< protects list mutations (cold path only)
} InstancesListV2;

/* ── Per-object envelope ──────────────────────────────────────────────────── */

/*! Management header prefixed to every pool-allocated object.
 *
 *  Cache-line notes:
 *    _refcount, holder_word, referenced, last_used_ts — hot path (same line)
 *    oid — cold (set once at init, read rarely)
 *    holder_fallback — cold (NULL unless RecyclerV2GetNewInstance called)
 */
typedef struct RecyclerV2PoolTypeEnvelop {
    _Atomic size_t      _refcount;        ///< 1=available, >=2=leased, put-back→1
    _Atomic uint32_t    next_free;        ///< Treiber stack next-pointer (dedicated field)
    size_t              oid;              ///< unique object ID, fixed for lifetime
    _Atomic uintptr_t   holder_word;      ///< single InstanceHolderV2 * (common case)
    _Atomic(InstancesListV2 *) holder_fallback; ///< Lazily installed multi-holder list
    _Atomic bool        referenced;       ///< CLOCK second-chance bit
    _Atomic bool        marshal_claimed;  ///< true while eviction serializes this object
    _Atomic uint64_t    last_used_ts;     ///< coarse timestamp, set on each Get
} RecyclerV2PoolTypeEnvelop;

/* ── Allocation group ─────────────────────────────────────────────────────── */

/*! A single allocation group — one contiguous memory chunk + Treiber free stack.
 *
 *  The free_stack_head is a tagged pointer:
 *    [63:32] pop_count  — increments on every pop (ABA protection)
 *    [31:0]  pool_index — index into this group's envelope array
 */
typedef struct AllocationGroupV2 {
    uint32_t            groupid;          ///< sequential group number (1-based)
    size_t              pool_size;        ///< number of envelopes in this group
    size_t              stride;           ///< aligned byte stride per envelope (cached for push/pop)
    void               *pool_memory;      ///< raw memory chunk (pool_size × stride)
    _Atomic uintptr_t   free_stack_head;  ///< Treiber stack: [63:32] pop_count, [31:0] pool_index
} AllocationGroupV2;

/* ── Per-type pool definition ─────────────────────────────────────────────── */

/*! Per-type pool definition — all metadata for one registered type.
 *
 *  Hot-path fields (accessed on every Get/Put):
 *    exp_seq, clock_hand, allocated_groups_sz, allocation_groups, pool_memory2
 *
 *  Cold-path fields (accessed on init/expansion/marshal):
 *    next_marshaller_id, pending_unmarshal_stack, storage_path
 */
typedef struct RecyclerV2PoolDefinition {
    /* Public handle (returned to consumer) */
    RecyclerV2PoolHandle    pool_handle;

    /* Callbacks */
    RecyclerV2PoolOps       ops;

    /* Configuration */
    float                   marshal_watermark;         ///< 0.0–1.0 trigger threshold (reserved)
    uint32_t                group_allocation_sz;       ///< max number of allocation groups
    uint32_t                expansion_threshold;       ///< items per group (= group pool_size)
    size_t                  marshal_blob_max_sz;       ///< consumer-declared max serialized size
    RecyclerV2StorageInitPolicy storage_init_policy;   ///< stale-blob handling on init

    /* Atomic synchronization */
    _Atomic uint32_t        exp_seq;                   ///< expansion generation (diagnostic)
    _Atomic bool            expanding;                 ///< CAS guard: one expander
    _Atomic uint64_t        clock_hand;                ///< CLOCK sweeper cursor (monotonic)
    _Atomic uint64_t        next_marshaller_id;        ///< monotonic ID allocator
    PendingUnmarshalQueueV2  pending_unmarshal_queue;   ///< mutex-protected queue for async unmarshal

    /* Group management */
    _Atomic uint32_t        allocated_groups_sz;       ///< published group count
    _Atomic size_t           current_max_capacity;      ///< published capacity

    void                  **pool_memory2;              ///< fixed directory, one raw chunk pointer per group
    AllocationGroupV2     **allocation_groups;         ///< fixed directory, never reallocated after init

    /* Storage */
    char                   *storage_root_ptr;           ///< stashed for config introspection (free'd separately)
    char                   *ufsrv_class_ptr;            ///< stashed ufsrv_class (NULL if not set)
    char                   *instance_id_ptr;            ///< stashed instance_id (NULL if not set)
    char                    storage_path[];             ///< <root>[/<instance>]/<type_name> — FAM
} RecyclerV2PoolDefinition;

/* ── Root singleton ───────────────────────────────────────────────────────── */

/*! Root singleton — holds all registered type pools. */
typedef struct RecyclerV2 {
    RecyclerV2PoolDefinition **pools_ptr;   ///< dynamic array, indexed by type_index - 1
    uint16_t                   count_typeslots; ///< allocated slots in pools_ptr[]
    uint16_t                   count_types;     ///< currently registered types
} RecyclerV2;

/* ── Accessor macros ──────────────────────────────────────────────────────── */

#define RECYCLER_V2_POOL_HANDLE_PTR(pool_ptr)   (&(pool_ptr)->pool_handle)
#define RECYCLER_V2_POOL_TYPENAME(pool_ptr)     ((pool_ptr)->pool_handle.type_name)
#define RECYCLER_V2_POOL_ALLOCATED_GROUPS_SZ(pool_ptr) ((pool_ptr)->allocated_groups_sz)
#define RECYCLER_V2_POOL_ALLOCATION_GROUP(pool_ptr, i) ((pool_ptr)->allocation_groups[i])

#define RECYCLER_V2_TYPE_INIT_CALLBACK(pool_ptr)       ((pool_ptr)->ops.poolop_init_callback)
#define RECYCLER_V2_TYPE_INITGET_CALLBACK(pool_ptr)    ((pool_ptr)->ops.poolop_initget_callback)
#define RECYCLER_V2_TYPE_INITPUT_CALLBACK(pool_ptr)    ((pool_ptr)->ops.poolop_initput_callback)
#define RECYCLER_V2_TYPE_DESTRUCT_CALLBACK(pool_ptr)   ((pool_ptr)->ops.poolop_destruct_callback)
#define RECYCLER_V2_TYPE_MARSHAL_CALLBACK(pool_ptr)    ((pool_ptr)->ops.poolop_marshal_callback)
#define RECYCLER_V2_TYPE_UNMARSHAL_CALLBACK(pool_ptr)  ((pool_ptr)->ops.poolop_unmarshal_callback)
#define RECYCLER_V2_TYPE_LAST_USED_CALLBACK(pool_ptr)  ((pool_ptr)->ops.poolop_last_used_callback)
#define RECYCLER_V2_TYPE_MARSHAL_CLEANUP_CALLBACK(pool_ptr) ((pool_ptr)->ops.poolop_marshal_cleanup_callback)

/* ── Envelope helpers ─────────────────────────────────────────────────────── */

/**
 * @brief Derive the envelope header from an InstanceHolderV2.
 *
 * The envelope is at a fixed negative offset from the object payload.
 *
 * @param ih_ptr  InstanceHolderV2 in Instance mode.
 * @return Pointer to the RecyclerV2PoolTypeEnvelop prefixed to the object.
 */
static inline RecyclerV2PoolTypeEnvelop *
sDeriveEnvelopeFromHolderV2(InstanceHolderV2 *ih_ptr)
{
    void *obj_ptr = RecyclerV2GetInstance(ih_ptr);
    return (RecyclerV2PoolTypeEnvelop *)((char *)obj_ptr - sizeof(RecyclerV2PoolTypeEnvelop));
}

/**
 * @brief Compute the aligned stride for pool objects.
 *
 * Each object occupies: sizeof(RecyclerV2PoolTypeEnvelop) + blocksz,
 * rounded up to _Alignof(RecyclerV2PoolTypeEnvelop) boundary.
 *
 * @param blocksz  sizeof(consumer object payload).
 * @return Aligned byte stride.
 */
static inline size_t
sEnvelopeStrideV2(size_t blocksz)
{
    size_t raw = sizeof(RecyclerV2PoolTypeEnvelop) + blocksz;
    size_t align = _Alignof(RecyclerV2PoolTypeEnvelop);
    return (raw + align - 1) & ~(align - 1);
}

/**
 * @brief Get the object payload pointer from an envelope.
 *
 * @param env_ptr  Envelope header.
 * @return Pointer to the object payload (ClientContextData).
 */
static inline void *
sEnvelopeObjectV2(RecyclerV2PoolTypeEnvelop *env_ptr)
{
    return (char *)env_ptr + sizeof(RecyclerV2PoolTypeEnvelop);
}

/* ── Macros ───────────────────────────────────────────────────────────────── */

#define IS_EMPTY_V2(ptr)  ((ptr) == NULL)

/** Byte offset of next_free within RecyclerV2PoolTypeEnvelop. */
#define RECYCLER_V2_NEXT_FREE_OFFSET  offsetof(RecyclerV2PoolTypeEnvelop, next_free)

#endif /* UFLIB_RECYCLER_V2_RECYCLER_V2_PRIV_H */
