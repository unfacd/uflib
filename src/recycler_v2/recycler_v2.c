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
 * @file recycler_v2.c
 * @brief RecyclerV2 — Lock-free slab allocator with marshaller-based object lifecycle
 *
 * Implementation of the V2 recycler: Treiber stack free queue, seqlock expansion,
 * atomic holder word, CLOCK second-chance marshaller, storage layer integration.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include "recycler_v2_priv.h"
#include "recycler_v2_storage_priv.h"
#include "recycler_v2_log_strings.h"

#include <uflib/recycler_v2/recycler_v2.h>
#include <uflib/cdt/cdt_treiber_stack.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/standard_c_includes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <inttypes.h>

/* ── Singleton ────────────────────────────────────────────────────────────── */

static RecyclerV2        sRecyclerV2 = {NULL, 0, 0};
static RecyclerV2 *const sRecyclerV2Ptr = &sRecyclerV2;

/* ── Forward declarations ─────────────────────────────────────────────────── */

static uint32_t              sFreeStackPopV2(AllocationGroupV2 *group_ptr);
static void                  sFreeStackPushV2(AllocationGroupV2 *group_ptr, uint32_t env_idx);
static void                  sFreeStackInitV2(AllocationGroupV2 *group_ptr);
static RecyclerV2PoolHandle *sTypePoolExpandAllocationGroupV2(RecyclerV2PoolDefinition *pool_ptr);
static RecyclerV2PoolTypeEnvelop *sClockScanVictimV2(RecyclerV2PoolDefinition *pool_ptr);
static InstanceHolderV2     *sMarshalVictimV2(RecyclerV2PoolDefinition *pool_ptr,
                                RecyclerV2PoolTypeEnvelop *victim_env_ptr);
static InstanceHolderV2 *sHolderAddV2(RecyclerV2PoolTypeEnvelop *env_ptr,
                                InstanceHolderV2 *ih_ptr);
static bool sHolderRemoveV2(RecyclerV2PoolTypeEnvelop *env_ptr,
                                InstanceHolderV2 *ih_ptr);
static void sPendingStackDrainV2(RecyclerV2PoolDefinition *pool_ptr);
static uint64_t sCoarseTimestamp(void);

/* ── Treiber stack — thin wrappers around the shared CDT module ──────────── */

/**
 * @brief Pop a free object index from the group's Treiber stack.
 *
 * Delegates to CDT_TreiberStackPop() — the canonical ABA-protected
 * implementation shared with LocklessFixedWidthHashMap.
 *
 * @param group_ptr  Allocation group.
 * @return Pool index (>=1), or 0 if the stack is empty (index 0 = sentinel).
 */
static uint32_t
sFreeStackPopV2(AllocationGroupV2 *group_ptr)
{
    return CDT_TreiberStackPop(&group_ptr->free_stack_head,
                               group_ptr->pool_memory,
                               group_ptr->stride,
                               RECYCLER_V2_NEXT_FREE_OFFSET);
}

/**
 * @brief Push a free object index onto the group's Treiber stack.
 *
 * Delegates to CDT_TreiberStackPush().
 *
 * @param group_ptr  Allocation group.
 * @param env_idx    Pool index to push (must be >= 1).
 */
static void
sFreeStackPushV2(AllocationGroupV2 *group_ptr, uint32_t env_idx)
{
    CDT_TreiberStackPush(&group_ptr->free_stack_head,
                         group_ptr->pool_memory,
                         group_ptr->stride,
                         RECYCLER_V2_NEXT_FREE_OFFSET,
                         env_idx);
}

/**
 * @brief Populate the Treiber stack with all pool indices in reverse order.
 *
 * Index 0 is the EMPTY sentinel — only indices 1..pool_size-1 are pushed.
 *
 * @param group_ptr  Allocation group (pool_memory, pool_size, stride must be set).
 */
static void
sFreeStackInitV2(AllocationGroupV2 *group_ptr)
{
    atomic_store_explicit(&group_ptr->free_stack_head, 0, memory_order_relaxed);

    /* Init all envelopes (including sentinel index 0) */
    for (size_t i = 0; i < group_ptr->pool_size; i++) {
        RecyclerV2PoolTypeEnvelop *env_ptr =
            (RecyclerV2PoolTypeEnvelop *)((char *)group_ptr->pool_memory
                + i * group_ptr->stride);

        atomic_store_explicit(&env_ptr->_refcount,
            (i == 0) ? 0 : 1, memory_order_relaxed);
        atomic_store_explicit(&env_ptr->next_free, 0, memory_order_relaxed);
        env_ptr->oid = (i == 0) ? 0
            : (size_t)((group_ptr->groupid << 20) | i);
        atomic_store_explicit(&env_ptr->holder_word, 0, memory_order_relaxed);
        atomic_store_explicit(&env_ptr->holder_fallback, NULL, memory_order_relaxed);
        atomic_store_explicit(&env_ptr->referenced, false, memory_order_relaxed);
        atomic_store_explicit(&env_ptr->marshal_claimed, false, memory_order_relaxed);
        atomic_store_explicit(&env_ptr->last_used_ts, 0, memory_order_relaxed);
    }

    /* Push usable indices (1..pool_size-1) onto the free stack in reverse
     * order so that the first pop gets index 1. */
    for (size_t i = group_ptr->pool_size - 1; i > 0; i--) {
        sFreeStackPushV2(group_ptr, (uint32_t)i);
    }
}

/* ── Coarse timestamp ─────────────────────────────────────────────────────── */

static uint64_t
sCoarseTimestamp(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    return 0;
}

/* ── Seqlock expansion ────────────────────────────────────────────────────── */

/**
 * @brief Expand a type pool by adding a new allocation group.
 *
 * Seqlock protocol (exp_seq):
 *   even = stable — readers can scan groups safely
 *   odd  = expansion in progress — readers retry
 *
 * The writer (this function) bumps exp_seq to odd, allocates, appends,
 * and bumps exp_seq back to even.  Appending is safe because:
 *   - The groups array is realloc'd but previous entries are unchanged
 *   - allocated_groups_sz is only incremented after all writes are visible
 *   - Readers that see old allocated_groups_sz miss the new group and may
 *     trigger a redundant expansion, which fails harmlessly (capacity check)
 *
 * @param pool_ptr  Pool definition.
 * @return Pool handle on success, NULL on failure (max groups reached or OOM).
 */
static RecyclerV2PoolHandle *
sTypePoolExpandAllocationGroupV2(RecyclerV2PoolDefinition *pool_ptr)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&pool_ptr->expanding, &expected, true,
            memory_order_acq_rel, memory_order_relaxed)) return NULL;
    atomic_fetch_add_explicit(&pool_ptr->exp_seq, 1, memory_order_acq_rel);

    uint32_t old_sz = atomic_load_explicit(&pool_ptr->allocated_groups_sz, memory_order_acquire);
    if (old_sz >= pool_ptr->group_allocation_sz) goto fail;

    AllocationGroupV2 *group_ptr = calloc(1, sizeof(*group_ptr));
    if (!group_ptr) goto fail;
    group_ptr->groupid = old_sz + 1;
    group_ptr->pool_size = pool_ptr->expansion_threshold;
    group_ptr->stride = sEnvelopeStrideV2(pool_ptr->pool_handle.blocksz);
    group_ptr->pool_memory = calloc(group_ptr->pool_size, group_ptr->stride);
    if (!group_ptr->pool_memory) { free(group_ptr); goto fail; }
    sFreeStackInitV2(group_ptr);

    if (RECYCLER_V2_TYPE_INIT_CALLBACK(pool_ptr)) {
        for (size_t i = 1; i < group_ptr->pool_size; ++i) {
            RecyclerV2PoolTypeEnvelop *env = (RecyclerV2PoolTypeEnvelop *)
                ((char *)group_ptr->pool_memory + i * group_ptr->stride);
            void *obj = (char *)env + sizeof(*env);
            if (pool_ptr->ops.poolop_init_callback((ClientContextData *)obj, env->oid) != 0) {
                free(group_ptr->pool_memory); free(group_ptr); goto fail;
            }
        }
    }
    pool_ptr->allocation_groups[old_sz] = group_ptr;
    pool_ptr->pool_memory2[old_sz] = group_ptr->pool_memory;
    atomic_store_explicit(&pool_ptr->allocated_groups_sz, old_sz + 1, memory_order_release);
    atomic_store_explicit(&pool_ptr->current_max_capacity,
        (size_t)(old_sz + 1) * pool_ptr->expansion_threshold, memory_order_release);
    atomic_fetch_add_explicit(&pool_ptr->exp_seq, 1, memory_order_release);
    atomic_store_explicit(&pool_ptr->expanding, false, memory_order_release);
    return RECYCLER_V2_POOL_HANDLE_PTR(pool_ptr);
fail:
    atomic_fetch_add_explicit(&pool_ptr->exp_seq, 1, memory_order_release);
    atomic_store_explicit(&pool_ptr->expanding, false, memory_order_release);
    return NULL;
}

/* ── Public: InitTypePool ────────────────────────────────────────────────── */

PUBLIC_API RecyclerV2PoolHandle *
RecyclerV2InitTypePool(const RecyclerV2PoolConfig *config_ptr)
{
    if (!config_ptr || !config_ptr->type_name || config_ptr->blocksz == 0) {
        return NULL;
    }
    if (sRecyclerV2Ptr->count_types == UINT16_MAX) {
        return NULL;
    }

    /* Resize the type registry if full */
    if (sRecyclerV2Ptr->count_types >= sRecyclerV2Ptr->count_typeslots) {
        uint16_t new_slots = sRecyclerV2Ptr->count_typeslots
            + CONFIG_DEFAULT_RECYCLER_V2_TYPE_SLOT_EXPANSION;
        RecyclerV2PoolDefinition **new_pools =
            (RecyclerV2PoolDefinition **)realloc(sRecyclerV2Ptr->pools_ptr,
                (size_t)new_slots * sizeof(RecyclerV2PoolDefinition *));
        if (!new_pools) return NULL;
        sRecyclerV2Ptr->pools_ptr = new_pools;
        sRecyclerV2Ptr->count_typeslots = new_slots;
    }

    /* Use consumer-specified storage root, or the compile-time default */
    const char *storage_root = config_ptr->storage_root
        ? config_ptr->storage_root
        : CONFIG_DEFAULT_RECYCLER_V2_STORAGE_ROOT;

    /* Build storage path: <root>[/<ufsrv_class>][/<instance_id>]/<type_name> */
    const char *srv_class  = config_ptr->ufsrv_class;
    const char *instance   = config_ptr->instance_id;
    size_t name_len = strlen(config_ptr->type_name);
    size_t storage_path_len = strlen(storage_root) + 1 + name_len + 1;
    if (srv_class)  storage_path_len += strlen(srv_class) + 1;
    if (instance)   storage_path_len += strlen(instance) + 1;

    /* Allocate pool definition with flexible storage_path */
    RecyclerV2PoolDefinition *pool_ptr = (RecyclerV2PoolDefinition *)calloc(1,
        sizeof(RecyclerV2PoolDefinition) + storage_path_len);
    if (!pool_ptr) return NULL;

    /* Populate public handle */
    pool_ptr->pool_handle.type = (uint16_t)(sRecyclerV2Ptr->count_types + 1);
    pool_ptr->pool_handle.type_name = strdup(config_ptr->type_name);
    pool_ptr->pool_handle.blocksz = config_ptr->blocksz;

    /* Populate config */
    pool_ptr->ops = config_ptr->ops_ptr ? *config_ptr->ops_ptr
                    : (RecyclerV2PoolOps){0};
    pool_ptr->marshal_watermark = (config_ptr->marshal_watermark > 0.0f)
        ? config_ptr->marshal_watermark
        : CONFIG_DEFAULT_RECYCLER_V2_MARSHAL_WATERMARK;
    pool_ptr->group_allocation_sz = config_ptr->group_allocation_sz
        ? config_ptr->group_allocation_sz
        : CONFIG_DEFAULT_RECYCLER_V2_MAX_ALLOCATION_GROUPS;
    pool_ptr->expansion_threshold = config_ptr->expansion_threshold
        ? config_ptr->expansion_threshold
        : CONFIG_DEFAULT_RECYCLER_V2_EXPANSION_THRESHOLD;
    pool_ptr->marshal_blob_max_sz = config_ptr->marshal_blob_max_sz;
    pool_ptr->storage_init_policy = config_ptr->storage_init_policy;
    /* Borrowed, never owned: the pool lives for the process, so the caller's
       logger must outlive the process too — create it before any pool. */
    pool_ptr->uf_logger = config_ptr->logger_ptr;
    /* OVERWRITE (0) is both the enum default and the safe behaviour — no
     * ambiguity: a zero-initialized config inherently means OVERWRITE. */

    /* Init atomics */
    atomic_store_explicit(&pool_ptr->exp_seq, 0, memory_order_relaxed);
    atomic_store_explicit(&pool_ptr->allocated_groups_sz, 0, memory_order_relaxed);
    atomic_store_explicit(&pool_ptr->current_max_capacity, 0, memory_order_relaxed);
    atomic_store_explicit(&pool_ptr->expanding, false, memory_order_relaxed);
    atomic_store_explicit(&pool_ptr->clock_hand, 0, memory_order_relaxed);
    atomic_store_explicit(&pool_ptr->next_marshaller_id, 1, memory_order_relaxed);
    pthread_mutex_init(&pool_ptr->pending_unmarshal_queue.mutex, NULL);
    pool_ptr->pending_unmarshal_queue.head_ptr = NULL;
    pool_ptr->pending_unmarshal_queue.tail_ptr = NULL;
    pool_ptr->pending_unmarshal_queue.count = 0;

    /* Build storage path and stash config strings for introspection.
     * Path: <root>[/<ufsrv_class>][/<instance_id>]/<type_name> */
    pool_ptr->storage_root_ptr = strdup(storage_root);
    pool_ptr->ufsrv_class_ptr  = srv_class ? strdup(srv_class) : NULL;
    pool_ptr->instance_id_ptr  = instance ? strdup(instance) : NULL;
    if (srv_class && instance) {
        snprintf(pool_ptr->storage_path, storage_path_len, "%s/%s/%s/%s",
            storage_root, srv_class, instance, config_ptr->type_name);
    } else if (srv_class) {
        snprintf(pool_ptr->storage_path, storage_path_len, "%s/%s/%s",
            storage_root, srv_class, config_ptr->type_name);
    } else if (instance) {
        snprintf(pool_ptr->storage_path, storage_path_len, "%s/%s/%s",
            storage_root, instance, config_ptr->type_name);
    } else {
        snprintf(pool_ptr->storage_path, storage_path_len, "%s/%s",
            storage_root, config_ptr->type_name);
    }

    /* Initialize storage layer — non-fatal if it fails.
     * In test/dev environments without writable /var/run, the recycler
     * operates normally but marshalling is unavailable. */
    RecyclerV2StorageInit(pool_ptr, pool_ptr->storage_init_policy);

    pool_ptr->allocation_groups = calloc(pool_ptr->group_allocation_sz, sizeof(*pool_ptr->allocation_groups));
    pool_ptr->pool_memory2 = calloc(pool_ptr->group_allocation_sz, sizeof(*pool_ptr->pool_memory2));
    if (!pool_ptr->allocation_groups || !pool_ptr->pool_memory2) {
        free(pool_ptr->allocation_groups); free(pool_ptr->pool_memory2);
        RecyclerV2StorageDestroy(pool_ptr);
        free((void *)pool_ptr->pool_handle.type_name); free(pool_ptr); return NULL;
    }

    /* Allocate the first group */
    RecyclerV2PoolHandle *handle =
        sTypePoolExpandAllocationGroupV2(pool_ptr);
    if (!handle) {
        RecyclerV2StorageDestroy(pool_ptr);
        free(pool_ptr->allocation_groups);
        free(pool_ptr->pool_memory2);
        free((void *)pool_ptr->pool_handle.type_name);
        free(pool_ptr);
        return NULL;
    }

    /* Register */
    sRecyclerV2Ptr->pools_ptr[pool_ptr->pool_handle.type - 1] = pool_ptr;
    sRecyclerV2Ptr->count_types++;

#if UF_DEBUG_BUILD
    syslog(LOG_DEBUG, "%s: pool registered type='%s' type_index=%u blocksz=%zu "
        "groups=%u threshold=%u watermark=%.2f blob_max=%zu storage='%s'",
        __func__, pool_ptr->pool_handle.type_name,
        pool_ptr->pool_handle.type, pool_ptr->pool_handle.blocksz,
        pool_ptr->group_allocation_sz, pool_ptr->expansion_threshold,
        (double)pool_ptr->marshal_watermark, pool_ptr->marshal_blob_max_sz,
        pool_ptr->storage_path);
#endif

    return RECYCLER_V2_POOL_HANDLE_PTR(pool_ptr);
}

/* ── Holder word operations ───────────────────────────────────────────────── */

/**
 * @brief Add an InstanceHolderV2 to an object's holder word.
 *
 * Fast path (common case): CAS NULL → holder_ptr on holder_word.
 * Slow path (cold, only if RecyclerV2GetNewInstance called): allocate
 * fallback InstancesListV2 and add both holders.
 *
 * @param env_ptr  Object envelope.
 * @param ih_ptr   InstanceHolderV2 to add.
 * @return ih_ptr on success, NULL on failure.
 */
static InstanceHolderV2 *
sHolderAddV2(RecyclerV2PoolTypeEnvelop *env_ptr, InstanceHolderV2 *ih_ptr)
{
    uintptr_t expected = 0;
    uintptr_t desired = (uintptr_t)ih_ptr;
    if (atomic_compare_exchange_strong_explicit(&env_ptr->holder_word,
            &expected, desired, memory_order_acq_rel, memory_order_acquire)) {
        return ih_ptr;
    }

    InstancesListV2 *list = atomic_load_explicit(&env_ptr->holder_fallback, memory_order_acquire);
    if (!list) {
        InstancesListV2 *candidate = (InstancesListV2 *)calloc(1, sizeof(*candidate));
        if (!candidate) return NULL;
        pthread_spin_init(&candidate->spin_lock, PTHREAD_PROCESS_PRIVATE);
        InstancesListV2 *expected_list = NULL;
        if (!atomic_compare_exchange_strong_explicit(&env_ptr->holder_fallback,
                &expected_list, candidate, memory_order_release, memory_order_acquire)) {
            pthread_spin_destroy(&candidate->spin_lock);
            free(candidate);
            list = expected_list;
        } else {
            list = candidate;
        }
    }

    pthread_spin_lock(&list->spin_lock);
    ListItemInstanceV2 *item = (ListItemInstanceV2 *)calloc(1, sizeof(*item));
    if (!item) {
        pthread_spin_unlock(&list->spin_lock);
        return NULL;
    }
    item->holder_ptr = ih_ptr;
    if (!list->head_ptr) list->head_ptr = list->tail_ptr = item;
    else { list->tail_ptr->next_ptr = item; list->tail_ptr = item; }
    list->size++;
    pthread_spin_unlock(&list->spin_lock);
    return ih_ptr;
}

static bool
sHolderRemoveV2(RecyclerV2PoolTypeEnvelop *env_ptr, InstanceHolderV2 *ih_ptr)
{
    uintptr_t expected = (uintptr_t)ih_ptr;
    if (atomic_compare_exchange_strong_explicit(&env_ptr->holder_word,
            &expected, 0, memory_order_acq_rel, memory_order_acquire)) {
        /* If aliases exist, promote one atomically under the fallback lock. */
        InstancesListV2 *list = atomic_load_explicit(&env_ptr->holder_fallback, memory_order_acquire);
        if (list) {
            pthread_spin_lock(&list->spin_lock);
            if (list->head_ptr) {
                ListItemInstanceV2 *n = list->head_ptr;
                list->head_ptr = n->next_ptr;
                if (!list->head_ptr) list->tail_ptr = NULL;
                list->size--;
                InstanceHolderV2 *promoted = n->holder_ptr;
                if (promoted) {
                    atomic_store_explicit(&env_ptr->holder_word, (uintptr_t)promoted, memory_order_release);
                } else {
                    /* Do not lose the alias if promotion allocation fails. */
                    n->next_ptr = list->head_ptr;
                    list->head_ptr = n;
                    if (!list->tail_ptr) list->tail_ptr = n;
                    list->size++;
                }
                if (promoted) free(n);
            }
            pthread_spin_unlock(&list->spin_lock);
        }
        return true;
    }

    InstancesListV2 *list = atomic_load_explicit(&env_ptr->holder_fallback, memory_order_acquire);
    if (!list) return false;
    pthread_spin_lock(&list->spin_lock);
    ListItemInstanceV2 *prev = NULL, *curr = list->head_ptr;
    while (curr) {
        if (curr->holder_ptr == ih_ptr) {
            if (prev) prev->next_ptr = curr->next_ptr; else list->head_ptr = curr->next_ptr;
            if (curr == list->tail_ptr) list->tail_ptr = prev;
            list->size--;
            free(curr);
            pthread_spin_unlock(&list->spin_lock);
            return true;
        }
        prev = curr; curr = curr->next_ptr;
    }
    pthread_spin_unlock(&list->spin_lock);
    return false;
}

/* ── Public: Get ─────────────────────────────────────────────────────────── */

PUBLIC_API InstanceHolderV2 *
RecyclerV2Get(RecyclerV2PoolHandle *handle_ptr, ContextData *context_data,
              unsigned long call_flags)
{
    if (!handle_ptr || handle_ptr->type == 0
        || handle_ptr->type > sRecyclerV2Ptr->count_types) {
        return NULL;
    }

    RecyclerV2PoolDefinition *pool_ptr =
        sRecyclerV2Ptr->pools_ptr[handle_ptr->type - 1];
    if (IS_EMPTY_V2(pool_ptr)) return NULL;

retry_get:
    /* Seqlock snapshot — read exp_seq before scanning groups */
    uint32_t seq_before = atomic_load_explicit(&pool_ptr->exp_seq,
                                               memory_order_acquire);
    if (seq_before & 1U) {
        /* Expansion in progress — spin briefly and retry */
        goto retry_get;
    }

    int groups_snap = atomic_load_explicit(&pool_ptr->allocated_groups_sz, memory_order_acquire);

    /* Scan groups for a free slot */
    AllocationGroupV2 *group_ptr = NULL;
    uint32_t env_idx = 0;  /* 0 = sentinel / stack-empty */

    for (int i = 0; i < groups_snap; i++) {
        AllocationGroupV2 *g = pool_ptr->allocation_groups[i];
        env_idx = sFreeStackPopV2(g);
        if (env_idx != 0) {
            group_ptr = g;
            break;
        }
    }

    if (!group_ptr) {
        /* All groups fully leased */

#if UF_DEBUG_BUILD
        syslog(LOG_DEBUG, "%s: fully leased type='%s' groups=%d capacity=%zu",
            __func__, pool_ptr->pool_handle.type_name,
            atomic_load_explicit(&pool_ptr->allocated_groups_sz, memory_order_acquire), atomic_load_explicit(&pool_ptr->current_max_capacity, memory_order_acquire));
#endif

        /* Try expansion first — it's fast (calloc + init) and
         * guaranteed to create free slots.  Only when expansion is
         * impossible (max groups or OOM) do we fall back to the
         * CLOCK marshaller, which must find idle (refcount==1)
         * objects among the currently-busy pool.
         *
         * Expansion returns NULL on: (a) another thread already
         * expanding → retry (b) max groups reached → try marshal
         * (c) OOM → try marshal.
         * Case (a) is transient — retrying Get may find the new
         * group the winner just appended. */
        if (sTypePoolExpandAllocationGroupV2(pool_ptr)) {
            goto retry_get;
        }
        if (atomic_load_explicit(&pool_ptr->expanding, memory_order_acquire)) {
            /* Another thread is mid-expansion — spin briefly, then
             * retry Get (will see the winner's new group). */
            goto retry_get;
        }

        /* Expansion blocked — max groups reached or OOM.
         * Try the CLOCK marshaller as a last resort: find an idle
         * object (refcount==1) and serialize it to storage to free
         * its slot.  Requires both a marshal callback AND a
         * consumer-declared max blob size (marshal_blob_max_sz > 0). */
        if (pool_ptr->ops.poolop_marshal_callback
            && pool_ptr->marshal_blob_max_sz > 0) {
            RecyclerV2PoolTypeEnvelop *victim = sClockScanVictimV2(pool_ptr);
            if (victim) {
                InstanceHolderV2 *marshalled_ih =
                    sMarshalVictimV2(pool_ptr, victim);
                if (marshalled_ih) {
                    goto retry_get;  // freed slot now available
                }
            }
        }

        return NULL;  // hard exhaustion
    }

    /* Got a free slot — validate seqlock */
    uint32_t seq_after = atomic_load_explicit(&pool_ptr->exp_seq,
                                              memory_order_relaxed);
    if (seq_after != seq_before) {
        /* Expansion happened during our scan — push back and retry */
        sFreeStackPushV2(group_ptr, env_idx);
        goto retry_get;
    }

    /* Derive envelope and bump refcount */
    size_t stride = sEnvelopeStrideV2(pool_ptr->pool_handle.blocksz);
    RecyclerV2PoolTypeEnvelop *env_ptr =
        (RecyclerV2PoolTypeEnvelop *)((char *)group_ptr->pool_memory
            + (size_t)env_idx * stride);

    /* Allocate the return holder FIRST — this is what holder_word points to.
     * MUST be heap-allocated because the stack-local address would become
     * invalid when this function returns, corrupting holder_word. */
    InstanceHolderV2 *ret = (InstanceHolderV2 *)malloc(sizeof(InstanceHolderV2));
    if (!ret) {
        sFreeStackPushV2(group_ptr, env_idx);
        return NULL;
    }

    /* Refcount: available(1) → leased(2) */
    atomic_fetch_add_explicit(&env_ptr->_refcount, 1, memory_order_acq_rel);

    void *obj_ptr = (char *)env_ptr + sizeof(RecyclerV2PoolTypeEnvelop);
    RecyclerV2SetInstance(ret, obj_ptr);

    /* Add holder to envelope — holder_word now stores heap pointer */
    InstanceHolderV2 *added = sHolderAddV2(env_ptr, ret);
    if (!added) {
        atomic_fetch_sub_explicit(&env_ptr->_refcount, 1, memory_order_acq_rel);
        sFreeStackPushV2(group_ptr, env_idx);
        free(ret);
        return NULL;
    }

    /* Set CLOCK referenced bit */
    atomic_store_explicit(&env_ptr->referenced, true, memory_order_relaxed);
    atomic_store_explicit(&env_ptr->last_used_ts, sCoarseTimestamp(),
                          memory_order_relaxed);

    /* Call initget callback */
    if (RECYCLER_V2_TYPE_INITGET_CALLBACK(pool_ptr)) {
        if (pool_ptr->ops.poolop_initget_callback(ret, context_data,
                env_ptr->oid, call_flags) != 0) {
            /* Callback rejected — undo and return NULL */
#if UF_DEBUG_BUILD
            syslog(LOG_DEBUG, "%s: initget rejected oid=%zu type='%s'",
                __func__, env_ptr->oid, pool_ptr->pool_handle.type_name);
#endif
            sHolderRemoveV2(env_ptr, ret);
            atomic_fetch_sub_explicit(&env_ptr->_refcount, 1,
                                      memory_order_acq_rel);
            sFreeStackPushV2(group_ptr, env_idx);
            free(ret);
            return NULL;
        }
    }

#if UF_DEBUG_BUILD
    syslog(LOG_DEBUG, "%s: oid=%zu env_idx=%u groupid=%u type='%s' "
        "refcount=%zu", __func__, env_ptr->oid, env_idx,
        group_ptr->groupid, pool_ptr->pool_handle.type_name,
        atomic_load(&env_ptr->_refcount));
#endif

    return ret;
}

/* ── Public: Put ─────────────────────────────────────────────────────────── */

PUBLIC_API int
RecyclerV2Put(InstanceHolderV2 *ih_ptr, ContextData *context_data,
              unsigned long call_flags)
{
    if (!ih_ptr) return -1;

    /* Reject marshalled holders */
    if (RecyclerV2IsMarshaller(ih_ptr)) {
        syslog(LOG_DEBUG, LOGSTR_RECYCLER_V2_MARSHALLER_INSTANCE, __func__,
            (unsigned long)pthread_self(), LOGCODE_RECYCLER_V2_MARSHALLER_INSTANCE,
            (void *)ih_ptr,
            (unsigned long)RecyclerV2GetMarshaller(ih_ptr));
        return -5;
    }

    /* Derive the envelope from the object pointer */
    void *obj_ptr = RecyclerV2GetInstance(ih_ptr);
    if (!obj_ptr) return -1;

    RecyclerV2PoolTypeEnvelop *env_ptr =
        (RecyclerV2PoolTypeEnvelop *)((char *)obj_ptr
            - sizeof(RecyclerV2PoolTypeEnvelop));

    if (atomic_load_explicit(&env_ptr->marshal_claimed, memory_order_acquire)) return -3;
    /* Find the pool — iterate to find which pool owns this envelope.
     * This is O(types) but type count is small (< 10 in practice). */
    RecyclerV2PoolDefinition *pool_ptr = NULL;
    AllocationGroupV2 *group_ptr = NULL;
    for (uint16_t t = 0; t < sRecyclerV2Ptr->count_types; t++) {
        RecyclerV2PoolDefinition *p = sRecyclerV2Ptr->pools_ptr[t];
        for (int g = 0; g < atomic_load_explicit(&p->allocated_groups_sz, memory_order_acquire); g++) {
            AllocationGroupV2 *grp = p->allocation_groups[g];
            size_t stride = sEnvelopeStrideV2(p->pool_handle.blocksz);
            char *start = (char *)grp->pool_memory;
            char *end = start + grp->pool_size * stride;
            if ((char *)env_ptr >= start && (char *)env_ptr < end) {
                pool_ptr = p;
                group_ptr = grp;
                goto found_pool;
            }
        }
    }
found_pool:
    if (!pool_ptr || !group_ptr) return -1;

    if (atomic_load_explicit(&env_ptr->marshal_claimed, memory_order_acquire)) return -3;
    uintptr_t expected_holder = (uintptr_t)ih_ptr;
    if (!atomic_compare_exchange_strong_explicit(&env_ptr->holder_word, &expected_holder, 0,
            memory_order_acq_rel, memory_order_acquire)) return -3;
    size_t refcount_read = atomic_load_explicit(&env_ptr->_refcount, memory_order_acquire);
    if (refcount_read != 2) {
        atomic_store_explicit(&env_ptr->holder_word, (uintptr_t)ih_ptr, memory_order_release);
        return -3;
    }

    /* Call initput callback */
    if (RECYCLER_V2_TYPE_INITPUT_CALLBACK(pool_ptr)) {
        pool_ptr->ops.poolop_initput_callback(ih_ptr, context_data, call_flags);
    }

    /* Reset refcount to available BEFORE pushing to the free stack.
     * Ordering is critical: if the push happens before the reset, a
     * concurrent Get can pop this object, bump refcount from 2→3, and
     * then the reset here clobbers it back to 1 — corrupting the
     * next holder's refcount. */
    atomic_store_explicit(&env_ptr->_refcount, 1, memory_order_release);

    /* Push back to free stack — now safe because refcount is already 1 */
    uint32_t env_idx = (uint32_t)(((char *)env_ptr - (char *)group_ptr->pool_memory)
        / sEnvelopeStrideV2(pool_ptr->pool_handle.blocksz));
    sFreeStackPushV2(group_ptr, env_idx);

    /* Opportunistically drain pending unmarshal queue */
    sPendingStackDrainV2(pool_ptr);

#if UF_DEBUG_BUILD
    syslog(LOG_DEBUG, "%s: oid=%zu env_idx=%u groupid=%u type='%s'",
        __func__, env_ptr->oid, env_idx, group_ptr->groupid,
        pool_ptr->pool_handle.type_name);
#endif

    return 0;
}

/* ── Reference counting ──────────────────────────────────────────────────── */

PUBLIC_API void
RecyclerV2Referenced(InstanceHolderV2 *ih_ptr, int multiples)
{
    if (!ih_ptr || RecyclerV2IsMarshaller(ih_ptr)) return;

    void *obj_ptr = RecyclerV2GetInstance(ih_ptr);
    if (!obj_ptr) return;

    RecyclerV2PoolTypeEnvelop *env_ptr =
        (RecyclerV2PoolTypeEnvelop *)((char *)obj_ptr
            - sizeof(RecyclerV2PoolTypeEnvelop));

    if (multiples <= 0) return;
    uintptr_t holder_word = (uintptr_t)ih_ptr;
    size_t rc = atomic_load_explicit(&env_ptr->_refcount, memory_order_acquire);
    while (rc >= 1) {
        if (atomic_load_explicit(&env_ptr->holder_word, memory_order_acquire) != holder_word) return;
        size_t desired = rc + (size_t)multiples;
        if (atomic_compare_exchange_weak_explicit(&env_ptr->_refcount, &rc, desired,
                memory_order_acq_rel, memory_order_acquire)) return;
    }
}

PUBLIC_API void
RecyclerV2UnReferenced(InstanceHolderV2 *ih_ptr, int multiples)
{
    if (!ih_ptr || RecyclerV2IsMarshaller(ih_ptr)) return;

    void *obj_ptr = RecyclerV2GetInstance(ih_ptr);
    if (!obj_ptr) return;

    RecyclerV2PoolTypeEnvelop *env_ptr =
        (RecyclerV2PoolTypeEnvelop *)((char *)obj_ptr
            - sizeof(RecyclerV2PoolTypeEnvelop));

    if (multiples <= 0) return;
    size_t n = (size_t)multiples;
    size_t rc = atomic_load_explicit(&env_ptr->_refcount, memory_order_acquire);
    while (rc > n && !atomic_compare_exchange_weak_explicit(&env_ptr->_refcount, &rc, rc - n,
                memory_order_acq_rel, memory_order_acquire)) {}
}

PUBLIC_API size_t
RecyclerV2GetReferenceCount(InstanceHolderV2 *ih_ptr)
{
    if (!ih_ptr || RecyclerV2IsMarshaller(ih_ptr)) return 0;

    void *obj_ptr = RecyclerV2GetInstance(ih_ptr);
    if (!obj_ptr) return 0;

    RecyclerV2PoolTypeEnvelop *env_ptr =
        (RecyclerV2PoolTypeEnvelop *)((char *)obj_ptr
            - sizeof(RecyclerV2PoolTypeEnvelop));

    return atomic_load_explicit(&env_ptr->_refcount, memory_order_acquire);
}

/* ── Client context extraction ────────────────────────────────────────────── */

PUBLIC_API ClientContextData *
RecyclerV2GetClientContextData(InstanceHolderV2 *ih_ptr)
{
    return (ClientContextData *)RecyclerV2GetInstance(ih_ptr);
}

PUBLIC_API InstanceHolderV2 *
RecyclerV2InstanceHolderFromClientContext(ContextData *ctx_ptr)
{
    if (!ctx_ptr) return NULL;

    /* Search all pools for the holder whose instance matches ctx_ptr */
    for (uint16_t t = 0; t < sRecyclerV2Ptr->count_types; t++) {
        RecyclerV2PoolDefinition *p = sRecyclerV2Ptr->pools_ptr[t];
        for (int g = 0; g < atomic_load_explicit(&p->allocated_groups_sz, memory_order_acquire); g++) {
            AllocationGroupV2 *grp = p->allocation_groups[g];
            size_t stride = sEnvelopeStrideV2(p->pool_handle.blocksz);
            for (size_t i = 0; i < grp->pool_size; i++) {
                RecyclerV2PoolTypeEnvelop *env_ptr =
                    (RecyclerV2PoolTypeEnvelop *)((char *)grp->pool_memory + i * stride);
                void *obj = (char *)env_ptr + sizeof(RecyclerV2PoolTypeEnvelop);
                if (obj == (void *)ctx_ptr) {
                    uintptr_t holder_word = atomic_load_explicit(
                        &env_ptr->holder_word, memory_order_acquire);
                    if (holder_word) {
                        return (InstanceHolderV2 *)holder_word;
                    }
                }
            }
        }
    }
    return NULL;
}

/* ── Multi-holder operations ──────────────────────────────────────────────── */

PUBLIC_API InstanceHolderV2 *
RecyclerV2GetNewInstance(InstanceHolderV2 *ih_ptr)
{
    if (!ih_ptr || RecyclerV2IsMarshaller(ih_ptr)) return NULL;

    void *obj_ptr = RecyclerV2GetInstance(ih_ptr);
    if (!obj_ptr) return NULL;

    RecyclerV2PoolTypeEnvelop *env_ptr =
        (RecyclerV2PoolTypeEnvelop *)((char *)obj_ptr
            - sizeof(RecyclerV2PoolTypeEnvelop));
    if (atomic_load_explicit(&env_ptr->marshal_claimed, memory_order_acquire)) return NULL;

    /* MUST be heap-allocated — sHolderAddV2 stores the pointer in holder_word
     * (or in the fallback list), so it must outlive this stack frame. */
    InstanceHolderV2 *new_holder_ptr =
        (InstanceHolderV2 *)malloc(sizeof(InstanceHolderV2));
    if (!new_holder_ptr) return NULL;

    RecyclerV2SetInstance(new_holder_ptr, obj_ptr);

    size_t rc = atomic_load_explicit(&env_ptr->_refcount, memory_order_acquire);
    do {
        if (rc == 0) { free(new_holder_ptr); return NULL; }
    } while (!atomic_compare_exchange_weak_explicit(&env_ptr->_refcount, &rc, rc + 1,
                 memory_order_acq_rel, memory_order_acquire));

    InstanceHolderV2 *added = sHolderAddV2(env_ptr, new_holder_ptr);
    if (!added) {
        atomic_fetch_sub_explicit(&env_ptr->_refcount, 1, memory_order_acq_rel);
        free(new_holder_ptr);
        return NULL;
    }
    return added;
}

PUBLIC_API int
RecyclerV2DestroyInstance(InstanceHolderV2 *ih_ptr)
{
    if (!ih_ptr) return RECYCLER_V2_INSTANCE_HOLDER_NOT_FOUND;

    void *obj_ptr = RecyclerV2GetInstance(ih_ptr);
    if (!obj_ptr) return RECYCLER_V2_INSTANCE_HOLDER_NOT_FOUND;

    RecyclerV2PoolTypeEnvelop *env_ptr =
        (RecyclerV2PoolTypeEnvelop *)((char *)obj_ptr
            - sizeof(RecyclerV2PoolTypeEnvelop));

    if (!sHolderRemoveV2(env_ptr, ih_ptr)) return RECYCLER_V2_INSTANCE_HOLDER_NOT_FOUND;
    size_t rc = atomic_load_explicit(&env_ptr->_refcount, memory_order_acquire);
    while (rc > 1 && !atomic_compare_exchange_weak_explicit(&env_ptr->_refcount, &rc, rc - 1,
                 memory_order_acq_rel, memory_order_acquire)) {}
    return RECYCLER_V2_INSTANCE_HOLDER_FOUND;
}

/* ── Marshaller configuration ─────────────────────────────────────────────── */

PUBLIC_API void
RecyclerV2SetMarshalWatermark(RecyclerV2PoolHandle *handle_ptr, float watermark)
{
    if (!handle_ptr || handle_ptr->type == 0
        || handle_ptr->type > sRecyclerV2Ptr->count_types) return;

    RecyclerV2PoolDefinition *pool_ptr =
        sRecyclerV2Ptr->pools_ptr[handle_ptr->type - 1];
    if (IS_EMPTY_V2(pool_ptr)) return;

    if (watermark < 0.0f) watermark = 0.0f;
    if (watermark > 1.0f) watermark = 1.0f;
    pool_ptr->marshal_watermark = watermark;
}

/* ── Query functions ──────────────────────────────────────────────────────── */

PUBLIC_API size_t
RecyclerV2GetCapacity(RecyclerV2PoolHandle *handle_ptr)
{
    if (!handle_ptr || handle_ptr->type == 0
        || handle_ptr->type > sRecyclerV2Ptr->count_types) return 0;

    RecyclerV2PoolDefinition *pool_ptr =
        sRecyclerV2Ptr->pools_ptr[handle_ptr->type - 1];
    if (IS_EMPTY_V2(pool_ptr)) return 0;

    return atomic_load_explicit(&pool_ptr->current_max_capacity, memory_order_acquire);
}

PUBLIC_API size_t
RecyclerV2GetLeasedCount(RecyclerV2PoolHandle *handle_ptr)
{
    if (!handle_ptr || handle_ptr->type == 0
        || handle_ptr->type > sRecyclerV2Ptr->count_types) return 0;

    RecyclerV2PoolDefinition *pool_ptr =
        sRecyclerV2Ptr->pools_ptr[handle_ptr->type - 1];
    if (IS_EMPTY_V2(pool_ptr)) return 0;

    size_t leased = 0;
    for (int g = 0; g < atomic_load_explicit(&pool_ptr->allocated_groups_sz, memory_order_acquire); g++) {
        AllocationGroupV2 *grp = pool_ptr->allocation_groups[g];
        size_t stride = sEnvelopeStrideV2(pool_ptr->pool_handle.blocksz);
        for (size_t i = 0; i < grp->pool_size; i++) {
            RecyclerV2PoolTypeEnvelop *env_ptr =
                (RecyclerV2PoolTypeEnvelop *)((char *)grp->pool_memory + i * stride);
            if (atomic_load_explicit(&env_ptr->_refcount, memory_order_relaxed) >= 2) {
                leased++;
            }
        }
    }
    return leased;
}

/* ── Config introspection ──────────────────────────────────────────────────── */

PUBLIC_API void
RecyclerV2GetPoolConfig(RecyclerV2PoolHandle *handle_ptr,
                        RecyclerV2PoolConfig *out_ptr)
{
    if (!handle_ptr || !out_ptr
        || handle_ptr->type == 0
        || handle_ptr->type > sRecyclerV2Ptr->count_types) {
        return;
    }

    RecyclerV2PoolDefinition *pool_ptr =
        sRecyclerV2Ptr->pools_ptr[handle_ptr->type - 1];
    if (IS_EMPTY_V2(pool_ptr)) return;

    out_ptr->type_name           = pool_ptr->pool_handle.type_name;
    out_ptr->blocksz             = pool_ptr->pool_handle.blocksz;
    out_ptr->group_allocation_sz = pool_ptr->group_allocation_sz;
    out_ptr->expansion_threshold = pool_ptr->expansion_threshold;
    out_ptr->ops_ptr             = &pool_ptr->ops;
    out_ptr->marshal_watermark   = pool_ptr->marshal_watermark;
    out_ptr->marshal_blob_max_sz = pool_ptr->marshal_blob_max_sz;
    out_ptr->storage_root        = pool_ptr->storage_root_ptr;
    out_ptr->ufsrv_class          = pool_ptr->ufsrv_class_ptr;
    out_ptr->instance_id          = pool_ptr->instance_id_ptr;
    out_ptr->storage_init_policy = pool_ptr->storage_init_policy;
}

/* ── CLOCK sweep ─────────────────────────────────────────────────────────── */

/**
 * @brief CLOCK second-chance victim selection.
 *
 * Algorithm:
 *   1. Advance clock_hand (monotonic, relaxed — CAS on free stack provides ordering)
 *   2. Wrap hand to current_max_capacity
 *   3. Scan up to PRIV_CONFIG_DEFAULT_RECYCLER_V2_CLOCK_MAX_SWEEP objects:
 *      a. Skip if refcount != 1 (in use or multi-referenced)
 *      b. If referenced == true: give second chance (CAS true→false), continue
 *      c. If referenced == false: victim found — return it
 *   4. If no victim after full sweep and poolop_last_used_callback exists:
 *      full O(n) scan for min last_used_ts
 *   5. Otherwise: return NULL (nothing marshallable)
 *
 * @param pool_ptr  Pool definition.
 * @return Victim envelope, or NULL if nothing marshallable.
 */
static RecyclerV2PoolTypeEnvelop *
sClockScanVictimV2(RecyclerV2PoolDefinition *pool_ptr)
{
    if (atomic_load_explicit(&pool_ptr->current_max_capacity, memory_order_acquire) == 0) return NULL;

    uint64_t hand = atomic_fetch_add_explicit(&pool_ptr->clock_hand, 1,
                                              memory_order_relaxed);
    size_t start = (size_t)(hand % atomic_load_explicit(&pool_ptr->current_max_capacity, memory_order_acquire));
    size_t stride = sEnvelopeStrideV2(pool_ptr->pool_handle.blocksz);
    size_t scanned = 0;
    size_t max_scan = PRIV_CONFIG_DEFAULT_RECYCLER_V2_CLOCK_MAX_SWEEP;
    if (max_scan > atomic_load_explicit(&pool_ptr->current_max_capacity, memory_order_acquire)) {
        max_scan = atomic_load_explicit(&pool_ptr->current_max_capacity, memory_order_acquire);
    }

    size_t pos = start;
    do {
        /* Find the group and offset for this position */
        for (int g = 0; g < atomic_load_explicit(&pool_ptr->allocated_groups_sz, memory_order_acquire); g++) {
            AllocationGroupV2 *grp = pool_ptr->allocation_groups[g];
            if (pos < grp->pool_size) {
                RecyclerV2PoolTypeEnvelop *env_ptr =
                    (RecyclerV2PoolTypeEnvelop *)((char *)grp->pool_memory + pos * stride);

                /* Check refcount — must be idle (refcount == 1) */
                size_t refcount = atomic_load_explicit(&env_ptr->_refcount,
                                                       memory_order_acquire);
                if (refcount != 1 || atomic_load_explicit(&env_ptr->marshal_claimed, memory_order_acquire)) {
                    pos = (pos + 1) % atomic_load_explicit(&pool_ptr->current_max_capacity, memory_order_acquire);
                    scanned++;
                    break;
                }

                /* Check referenced bit — second chance */
                bool referenced = true;
                if (atomic_compare_exchange_strong_explicit(&env_ptr->referenced,
                        &referenced, false, memory_order_relaxed,
                        memory_order_relaxed)) {
                    /* It was true — gave it a second chance */
                    pos = (pos + 1) % atomic_load_explicit(&pool_ptr->current_max_capacity, memory_order_acquire);
                    scanned++;
                    break;
                }
                /* referenced was already false — victim found */
                return env_ptr;
            }
            pos -= grp->pool_size;
        }
    } while (scanned < max_scan && pos != start);

    /* Full sweep fallback: last_used_callback tiebreaker */
    if (RECYCLER_V2_TYPE_LAST_USED_CALLBACK(pool_ptr)) {
        RecyclerV2PoolTypeEnvelop *best_env = NULL;
        uint64_t best_ts = UINT64_MAX;

        for (int g = 0; g < atomic_load_explicit(&pool_ptr->allocated_groups_sz, memory_order_acquire); g++) {
            AllocationGroupV2 *grp = pool_ptr->allocation_groups[g];
            for (size_t i = 0; i < grp->pool_size; i++) {
                RecyclerV2PoolTypeEnvelop *env_ptr =
                    (RecyclerV2PoolTypeEnvelop *)((char *)grp->pool_memory + i * stride);

                size_t refcount = atomic_load_explicit(&env_ptr->_refcount,
                                                       memory_order_acquire);
                if (refcount != 1 || atomic_load_explicit(&env_ptr->marshal_claimed, memory_order_acquire)) continue;

                uint64_t ts = pool_ptr->ops.poolop_last_used_callback(
                    (InstanceHolderV2 *)atomic_load_explicit(
                        &env_ptr->holder_word, memory_order_acquire));
                if (ts < best_ts) {
                    best_ts = ts;
                    best_env = env_ptr;
                }
            }
        }
        return best_env;
    }

    return NULL;  // nothing marshallable
}

/* ── Marshal path ────────────────────────────────────────────────────────── */

/**
 * @brief Marshal a victim object: serialize → persist → CAS holder.
 *
 * The recycler owns the entire I/O path:
 *   1. Allocate a buffer (stack, CONFIG_DEFAULT_RECYCLER_V2_MARSHAL_BLOB_MAX_SZ)
 *   2. Call poolop_marshal_callback → consumer serializes object into buffer
 *   3. Recycler writes buffer to filesystem (atomic tmp+rename)
 *   4. Encode the marshaller_id into the tagged pointer
 *   5. CAS holder->holder.marshaller: Instance_ptr → marshaller word
 *   6. If CAS success: push freed slot to Treiber stack
 *   7. If CAS failed: delete the written blob (another thread won the race)
 *
 * The consumer never touches a file descriptor.  The consumer never sees
 * the marshaller_id.  The consumer only serialises its object into the
 * provided buffer.
 *
 * @param pool_ptr        Pool definition.
 * @param victim_env_ptr  Envelope of the victim object.
 * @return The InstanceHolderV2 that was marshalled, or NULL on failure.
 */
static InstanceHolderV2 *
sMarshalVictimV2(RecyclerV2PoolDefinition *pool_ptr,
                 RecyclerV2PoolTypeEnvelop *victim_env_ptr)
{
    uintptr_t holder_word = atomic_load_explicit(&victim_env_ptr->holder_word, memory_order_acquire);
    if (!holder_word) return NULL;
    bool expected_claim = false;
    if (!atomic_compare_exchange_strong_explicit(&victim_env_ptr->marshal_claimed, &expected_claim, true,
            memory_order_acq_rel, memory_order_acquire)) return NULL;
    holder_word = atomic_load_explicit(&victim_env_ptr->holder_word, memory_order_acquire);
    if (!holder_word) { atomic_store_explicit(&victim_env_ptr->marshal_claimed, false, memory_order_release); return NULL; }
    InstanceHolderV2 *holder_ptr = (InstanceHolderV2 *)holder_word;
    void *obj_ptr = (char *)victim_env_ptr + sizeof(RecyclerV2PoolTypeEnvelop);

    /* Marshalling requires the consumer to declare a max blob size at pool
     * init time — only the consumer knows how large its serialized form is. */
    if (pool_ptr->marshal_blob_max_sz == 0) { atomic_store_explicit(&victim_env_ptr->marshal_claimed, false, memory_order_release); return NULL; }

    /* Step 1: Consumer serializes into a recycler-allocated heap buffer.
     * The size is consumer-declared (marshal_blob_max_sz in pool config),
     * so the buffer is always exactly the right size.  The consumer never
     * touches a file descriptor, never sees the storage path. */
    uint8_t *buf = (uint8_t *)malloc(pool_ptr->marshal_blob_max_sz);
    if (!buf) { atomic_store_explicit(&victim_env_ptr->marshal_claimed, false, memory_order_release); return NULL; }

    size_t blob_len = 0;

    if (RECYCLER_V2_TYPE_MARSHAL_CALLBACK(pool_ptr)) {
        if (pool_ptr->ops.poolop_marshal_callback(
                (ClientContextData *)obj_ptr, buf,
                pool_ptr->marshal_blob_max_sz, &blob_len) != 0) {
            syslog(LOG_DEBUG, LOGSTR_RECYCLER_V2_MARSHAL_FAILED, __func__,
                (unsigned long)pthread_self(), LOGCODE_RECYCLER_V2_MARSHAL_FAILED,
                pool_ptr->pool_handle.type_name,
                (unsigned long)victim_env_ptr->oid);
            free(buf);
            atomic_store_explicit(&victim_env_ptr->marshal_claimed, false, memory_order_release);
            return NULL;
        }
    }
    if (blob_len == 0 || blob_len > pool_ptr->marshal_blob_max_sz) {
        free(buf);
        atomic_store_explicit(&victim_env_ptr->marshal_claimed, false, memory_order_release);
        return NULL;
    }

    /* Step 2: Recycler writes blob to persistent storage.
     * Atomic write-tmp-then-rename, monotonic ID allocation.
     * After this call the buffer is no longer needed. */
    uint64_t storage_id = 0;
    int write_rc = RecyclerV2StorageWrite(pool_ptr, buf, blob_len, &storage_id);
    free(buf);

    if (write_rc != 0) {
        syslog(LOG_DEBUG, LOGSTR_RECYCLER_V2_MARSHAL_FAILED, __func__,
            (unsigned long)pthread_self(), LOGCODE_RECYCLER_V2_MARSHAL_FAILED,
            pool_ptr->pool_handle.type_name,
            (unsigned long)victim_env_ptr->oid);
        atomic_store_explicit(&victim_env_ptr->marshal_claimed, false, memory_order_release);
        return NULL;
    }

    /* Step 3: Encode the marshaller word and CAS the holder transition.
     * The marshaller_id is the storage_id — a monotonic counter the recycler
     * allocated during StorageWrite.  The consumer never sees this value. */
    uintptr_t marshalled_word = RecyclerV2EncodeMarshaller(
        pool_ptr->pool_handle.type, storage_id);

    uintptr_t old_word = atomic_load_explicit(&holder_ptr->holder.marshaller, memory_order_acquire);
    if (atomic_compare_exchange_strong_explicit(
            &holder_ptr->holder.marshaller,
            &old_word, marshalled_word,
            memory_order_acq_rel, memory_order_relaxed)) {
        /* CAS succeeded — object is now marshalled.
         * Push the freed envelope back to the free stack. */
        syslog(LOG_DEBUG, LOGSTR_RECYCLER_V2_MARSHAL_SUCCESS, __func__,
            (unsigned long)pthread_self(), LOGCODE_RECYCLER_V2_MARSHAL_SUCCESS,
            pool_ptr->pool_handle.type_name,
            (unsigned long)victim_env_ptr->oid, (unsigned long)storage_id);

        /* Establish free state before publishing the slot. */
        atomic_store_explicit(&victim_env_ptr->holder_word, 0, memory_order_release);
        atomic_store_explicit(&victim_env_ptr->_refcount, 1, memory_order_release);
        atomic_store_explicit(&victim_env_ptr->marshal_claimed, false, memory_order_release);

        /* Publish only after the entire free state is complete. */
        for (uint32_t g = 0; g < atomic_load_explicit(&pool_ptr->allocated_groups_sz, memory_order_acquire); g++) {
            AllocationGroupV2 *grp = pool_ptr->allocation_groups[g];
            size_t stride = sEnvelopeStrideV2(pool_ptr->pool_handle.blocksz);
            char *start = (char *)grp->pool_memory;
            char *end = start + grp->pool_size * stride;
            if ((char *)victim_env_ptr >= start && (char *)victim_env_ptr < end) {
                uint32_t idx = (uint32_t)(((char *)victim_env_ptr - start) / stride);
                sFreeStackPushV2(grp, idx);
                break;
            }
        }
        return holder_ptr;
    }

    /* CAS failed — another thread transitioned this holder first.
     * Delete the blob we just wrote (the winner's blob is the canonical one). */
    syslog(LOG_DEBUG, LOGSTR_RECYCLER_V2_MARSHAL_CAS_FAILED, __func__,
        (unsigned long)pthread_self(), LOGCODE_RECYCLER_V2_MARSHAL_CAS_FAILED,
        pool_ptr->pool_handle.type_name);
    RecyclerV2StorageDelete(pool_ptr, storage_id);
    atomic_store_explicit(&victim_env_ptr->marshal_claimed, false, memory_order_release);
    return NULL;
}

/* ── Unmarshal path ──────────────────────────────────────────────────────── */

/**
 * @brief Resolve a marshalled InstanceHolderV2 back to Instance mode.
 *
 * Called from RecyclerV2GetInstance() slow path.
 *
 * Algorithm (from design doc §6.5):
 *   1. Extract type_index and marshaller_id from holder word
 *   2. Pop a free object from the pool's Treiber stack
 *   3. If no free slot: try marshalling another victim, then retry pop
 *   4. Call poolop_unmarshal_callback → populate object
 *   5. CAS holder->holder.marshaller: marshalled_word → Instance_ptr
 *   6. If CAS success: delete storage, return object
 *   7. If CAS failed: push object back, re-read holder (now Instance), recurse
 *
 * @param ih_ptr  InstanceHolderV2 in Marshaller mode.
 * @return Object pointer, or NULL on failure.
 */
void *
RecyclerV2ResolveMarshalledInstance(InstanceHolderV2 *ih_ptr)
{
    uintptr_t word = atomic_load_explicit(&ih_ptr->holder.marshaller, memory_order_acquire);
    if ((word & 1) == 0) {
        /* Already resolved by another thread — fast path */
        return (void *)(word & RECYCLER_V2_INSTANCE_MASK);
    }

    uint16_t type_index = RecyclerV2MarshallerTypeIndex(word);
    uint64_t marshaller_id = RecyclerV2MarshallerId(word);

    if (type_index == 0 || type_index > sRecyclerV2Ptr->count_types) {
        return NULL;
    }

    RecyclerV2PoolDefinition *pool_ptr =
        sRecyclerV2Ptr->pools_ptr[type_index - 1];
    if (IS_EMPTY_V2(pool_ptr)) return NULL;

    syslog(LOG_DEBUG, LOGSTR_RECYCLER_V2_UNMARSHAL_TRIGGERED, __func__,
        (unsigned long)pthread_self(), LOGCODE_RECYCLER_V2_UNMARSHAL_TRIGGERED,
        (unsigned int)type_index, (unsigned long)marshaller_id);

    /* Pop a free object */
    AllocationGroupV2 *group_ptr = NULL;
    uint32_t env_idx = 0;  /* 0 = sentinel / stack-empty */

    for (int i = 0; i < atomic_load_explicit(&pool_ptr->allocated_groups_sz, memory_order_acquire); i++) {
        AllocationGroupV2 *g = pool_ptr->allocation_groups[i];
        env_idx = sFreeStackPopV2(g);
        if (env_idx != 0) {
            group_ptr = g;
            break;
        }
    }

    if (!group_ptr) {
        /* No free slot — try marshalling a victim to free one */
        RecyclerV2PoolTypeEnvelop *victim = sClockScanVictimV2(pool_ptr);
        if (victim) {
            sMarshalVictimV2(pool_ptr, victim);
            /* Retry pop */
            for (int i = 0; i < atomic_load_explicit(&pool_ptr->allocated_groups_sz, memory_order_acquire); i++) {
                AllocationGroupV2 *g = pool_ptr->allocation_groups[i];
                env_idx = sFreeStackPopV2(g);
                if (env_idx != 0) {
                    group_ptr = g;
                    break;
                }
            }
        }
        if (!group_ptr) return NULL;  // truly exhausted
    }

    size_t stride = sEnvelopeStrideV2(pool_ptr->pool_handle.blocksz);
    RecyclerV2PoolTypeEnvelop *env_ptr =
        (RecyclerV2PoolTypeEnvelop *)((char *)group_ptr->pool_memory
            + (size_t)env_idx * stride);

    void *obj_ptr = (char *)env_ptr + sizeof(RecyclerV2PoolTypeEnvelop);

    /* Step 1: Recycler reads the serialized blob from persistent storage.
     * The consumer never touches a file descriptor, never knows the path. */
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (RecyclerV2StorageRead(pool_ptr, marshaller_id, &blob, &blob_len) != 0) {
        syslog(LOG_DEBUG, LOGSTR_RECYCLER_V2_UNMARSHAL_FAILED, __func__,
            (unsigned long)pthread_self(), LOGCODE_RECYCLER_V2_UNMARSHAL_FAILED,
            (unsigned int)type_index, (unsigned long)marshaller_id);
        sFreeStackPushV2(group_ptr, env_idx);
        return NULL;
    }

    /* Step 2: Consumer deserializes from the blob into the fresh object.
     * The consumer never allocates — the object is already pre-allocated. */
    if (RECYCLER_V2_TYPE_UNMARSHAL_CALLBACK(pool_ptr)) {
        if (pool_ptr->ops.poolop_unmarshal_callback(
                (ClientContextData *)obj_ptr, blob, blob_len) != 0) {
            syslog(LOG_DEBUG, LOGSTR_RECYCLER_V2_UNMARSHAL_FAILED, __func__,
                (unsigned long)pthread_self(), LOGCODE_RECYCLER_V2_UNMARSHAL_FAILED,
                (unsigned int)type_index, (unsigned long)marshaller_id);
            free(blob);
            sFreeStackPushV2(group_ptr, env_idx);
            return NULL;
        }
    }

    /* Step 3: Recycler deletes the blob from persistent storage.
     * The re-materialized object is now the authoritative copy. */
    RecyclerV2StorageDelete(pool_ptr, marshaller_id);
    free(blob);

    if (RECYCLER_V2_TYPE_MARSHAL_CLEANUP_CALLBACK(pool_ptr)) {
        MarshallerContextData mctx = (MarshallerContextData)marshaller_id;
        pool_ptr->ops.poolop_marshal_cleanup_callback(mctx);
    }

    /* Step 4: CAS holder word: marshalled → Instance.
     * If another thread already resolved it, push our object back
     * and recurse (holder is now in Instance mode). */
    uintptr_t new_word = (uintptr_t)obj_ptr
        | (RECYCLER_V2_DEFAULT_TAG << RECYCLER_V2_TAG_SHIFT);

    if (atomic_compare_exchange_strong_explicit(
            &ih_ptr->holder.marshaller,
            &word, new_word,
            memory_order_acq_rel, memory_order_relaxed)) {
        atomic_store_explicit(&env_ptr->holder_word, (uintptr_t)ih_ptr,
                              memory_order_release);
        atomic_fetch_add_explicit(&env_ptr->_refcount, 1, memory_order_acq_rel);
        atomic_store_explicit(&env_ptr->referenced, true, memory_order_relaxed);
        atomic_store_explicit(&env_ptr->last_used_ts, sCoarseTimestamp(),
                              memory_order_relaxed);

        syslog(LOG_DEBUG, LOGSTR_RECYCLER_V2_UNMARSHAL_SUCCESS, __func__,
            (unsigned long)pthread_self(), LOGCODE_RECYCLER_V2_UNMARSHAL_SUCCESS,
            (unsigned int)type_index, (unsigned long)marshaller_id);

        return obj_ptr;
    }

    /* CAS failed — another thread already resolved it.
     * Push our object back and recurse (now Instance mode). */
    sFreeStackPushV2(group_ptr, env_idx);
    return RecyclerV2GetInstance(ih_ptr);
}

/* ── Pending unmarshal queue ─────────────────────────────────────────────── */

/**
 * @brief Enqueue a marshalled holder for later resolution.
 *
 * Called from RecyclerV2GetInstanceTry() when the holder is in Marshaller mode.
 * The pending queue is a mutex-protected linked list of heap-allocated
 * PendingUnmarshalNodeV2 wrappers.  InstanceHolderV2 is caller-allocated
 * so we cannot embed a next pointer directly.
 *
 * @param ih_ptr  InstanceHolderV2 in Marshaller mode (caller-allocated).
 */
void
RecyclerV2EnqueuePendingUnmarshal(InstanceHolderV2 *ih_ptr)
{
    uintptr_t word = atomic_load_explicit(&ih_ptr->holder.marshaller, memory_order_acquire);
    uint16_t type_index = RecyclerV2MarshallerTypeIndex(word);

    if (type_index == 0 || type_index > sRecyclerV2Ptr->count_types) return;

    RecyclerV2PoolDefinition *pool_ptr =
        sRecyclerV2Ptr->pools_ptr[type_index - 1];
    if (IS_EMPTY_V2(pool_ptr)) return;

    /* Allocate wrapper node to carry the holder pointer + next link */
    PendingUnmarshalNodeV2 *node_ptr =
        (PendingUnmarshalNodeV2 *)calloc(1, sizeof(PendingUnmarshalNodeV2));
    if (!node_ptr) return;

    node_ptr->holder_ptr = ih_ptr;
    node_ptr->next_ptr   = NULL;

    pthread_mutex_lock(&pool_ptr->pending_unmarshal_queue.mutex);

    if (!pool_ptr->pending_unmarshal_queue.head_ptr) {
        pool_ptr->pending_unmarshal_queue.head_ptr = node_ptr;
        pool_ptr->pending_unmarshal_queue.tail_ptr = node_ptr;
    } else {
        pool_ptr->pending_unmarshal_queue.tail_ptr->next_ptr = node_ptr;
        pool_ptr->pending_unmarshal_queue.tail_ptr = node_ptr;
    }
    pool_ptr->pending_unmarshal_queue.count++;

    pthread_mutex_unlock(&pool_ptr->pending_unmarshal_queue.mutex);
}

/**
 * @brief Pop all pending holders from the queue (bulk drain under lock).
 *
 * Atomically removes the entire linked list and returns the head.
 * The caller resolves each holder; any failure pushed back requires
 * a fresh enqueue.
 *
 * @param pool_ptr  Pool definition.
 * @return Head of the popped linked list, or NULL if queue empty.
 */
static PendingUnmarshalNodeV2 *
sPendingQueuePopAllV2(RecyclerV2PoolDefinition *pool_ptr)
{
    pthread_mutex_lock(&pool_ptr->pending_unmarshal_queue.mutex);

    PendingUnmarshalNodeV2 *head = pool_ptr->pending_unmarshal_queue.head_ptr;
    pool_ptr->pending_unmarshal_queue.head_ptr = NULL;
    pool_ptr->pending_unmarshal_queue.tail_ptr = NULL;
    pool_ptr->pending_unmarshal_queue.count = 0;

    pthread_mutex_unlock(&pool_ptr->pending_unmarshal_queue.mutex);
    return head;
}

/**
 * @brief Opportunistically drain the pending unmarshal queue.
 *
 * Called from RecyclerV2Put() after returning an object to the free stack.
 * Bulk-pops all pending holders under the mutex, then resolves each one.
 * Successfully resolved holders are now in Instance mode — consumers see
 * them on the next RecyclerV2GetInstance() call.  Unresolvable holders are
 * re-enqueued via RecyclerV2EnqueuePendingUnmarshal.
 *
 * @param pool_ptr  Pool definition.
 */
static void
sPendingStackDrainV2(RecyclerV2PoolDefinition *pool_ptr)
{
    PendingUnmarshalNodeV2 *node_ptr = sPendingQueuePopAllV2(pool_ptr);
    while (node_ptr) {
        PendingUnmarshalNodeV2 *next = node_ptr->next_ptr;
        InstanceHolderV2 *holder_ptr = node_ptr->holder_ptr;
        free(node_ptr);

        void *obj = RecyclerV2ResolveMarshalledInstance(holder_ptr);
        if (!obj) {
            /* Can't resolve now — re-enqueue for a future drain */
            RecyclerV2EnqueuePendingUnmarshal(holder_ptr);
        }
        /* Successfully resolved — holder is now Instance mode. */

        node_ptr = next;
    }
}

/* ── State dump (JSON introspection) ─────────────────────────────────────── */

static const char *
sSlotStateLabelV2(size_t refcount, uintptr_t holder_word)
{
    if (refcount == 0)  return "SENTINEL";
    if (refcount == 1)  return (holder_word != 0) ? "MARSHALLED" : "FREE";
    return "LEASED";
}

static const char *
sHolderModeLabelV2(uintptr_t holder_word)
{
    if (holder_word == 0) return "NONE";
    /* holder_word stores an InstanceHolderV2 * — check its marshaller bit */
    InstanceHolderV2 *ih = (InstanceHolderV2 *)holder_word;
    return RecyclerV2IsMarshaller(ih) ? "MARSHALLER" : "INSTANCE";
}

static void
sDumpJsonStringV2(FILE *s, const char *str)
{
    if (!str) { fputs("null", s); return; }
    fputc('"', s);
    for (const char *p = str; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", s); break;
        case '\\': fputs("\\\\", s); break;
        case '\n': fputs("\\n",  s); break;
        case '\t': fputs("\\t",  s); break;
        default:   fputc(*p, s);
        }
    }
    fputc('"', s);
}

static void
sDumpStorageInventoryV2(FILE *s, RecyclerV2PoolDefinition *pool_ptr)
{
    DIR *dir = opendir(pool_ptr->storage_path);
    if (!dir) { fputs("null", s); return; }

    fputc('[', s);
    bool first = true;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Expect filenames like 0000000000000001.pb */
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 3, ".pb") != 0) continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s",
            pool_ptr->storage_path, entry->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        /* Parse marshaller_id from hex prefix */
        uint64_t mid = strtoull(entry->d_name, NULL, 16);

        if (!first) fputc(',', s);
        first = false;
        fprintf(s,
            "\n          {"
            "\"marshaller_id\":%" PRIu64 ","
            "\"filename\":", mid);
        sDumpJsonStringV2(s, entry->d_name);
        fprintf(s, ",\"size_bytes\":%ld,\"mtime\":%ld}",
            (long)st.st_size, (long)st.st_mtime);
    }
    closedir(dir);

    if (!first) fputc('\n', s);
    fputs("        ]", s);
}

PUBLIC_API void
RecyclerV2DumpState(FILE *stream)
{
    FILE *s = stream ? stream : stdout;

    fprintf(s, "{\n  \"count_types\": %u,\n  \"pools\": [\n",
        sRecyclerV2Ptr->count_types);

    for (uint16_t t = 0; t < sRecyclerV2Ptr->count_types; t++) {
        RecyclerV2PoolDefinition *p = sRecyclerV2Ptr->pools_ptr[t];
        if (IS_EMPTY_V2(p)) continue;

        if (t > 0) fputs(",\n", s);

        /* ── Pool header ─────────────────────────────────────────────── */
        fprintf(s,
            "    {\n"
            "      \"type_index\": %u,\n",
            p->pool_handle.type);
        fputs("      \"type_name\": ", s);
        sDumpJsonStringV2(s, p->pool_handle.type_name);
        fprintf(s, ",\n");

        /* ── Config ──────────────────────────────────────────────────── */
        fputs("      \"config\": {\n", s);
        fprintf(s,
            "        \"blocksz\": %zu,\n"
            "        \"group_allocation_sz\": %u,\n"
            "        \"expansion_threshold\": %u,\n"
            "        \"marshal_watermark\": %.2f,\n"
            "        \"marshal_blob_max_sz\": %zu,\n",
            p->pool_handle.blocksz,
            p->group_allocation_sz,
            p->expansion_threshold,
            (double)p->marshal_watermark,
            p->marshal_blob_max_sz);
        fputs("        \"storage_root\": ", s);
        sDumpJsonStringV2(s, p->storage_root_ptr);
        fputs(",\n        \"instance_id\": ", s);
        sDumpJsonStringV2(s, p->instance_id_ptr);
        fputs(",\n        \"storage_init_policy\": ", s);
        sDumpJsonStringV2(s,
            p->storage_init_policy == RECYCLER_V2_STORAGE_OVERWRITE ? "OVERWRITE" :
            p->storage_init_policy == RECYCLER_V2_STORAGE_APPEND    ? "APPEND"    :
            "ARCHIVE");

        /* ── Capacity ────────────────────────────────────────────────── */
        size_t leased = 0, free_slots = 0, marshalled = 0;
        for (int g = 0; g < atomic_load_explicit(&p->allocated_groups_sz, memory_order_acquire); g++) {
            AllocationGroupV2 *grp = p->allocation_groups[g];
            size_t stride = sEnvelopeStrideV2(p->pool_handle.blocksz);
            for (size_t i = 0; i < grp->pool_size; i++) {
                RecyclerV2PoolTypeEnvelop *env =
                    (RecyclerV2PoolTypeEnvelop *)((char *)grp->pool_memory + i * stride);
                size_t rc = atomic_load(&env->_refcount);
                if (rc >= 2) leased++;
                else if (rc == 1) {
                    uintptr_t hw = atomic_load(&env->holder_word);
                    if (hw != 0) marshalled++;
                    else free_slots++;
                }
            }
        }
        fprintf(s,
            ",\n        \"storage_path\": ");
        sDumpJsonStringV2(s, p->storage_path);
        fprintf(s,
            "\n      },\n"
            "      \"capacity\": {\n"
            "        \"total\": %zu,\n"
            "        \"leased\": %zu,\n"
            "        \"free\": %zu,\n"
            "        \"marshalled\": %zu,\n"
            "        \"groups\": %d\n"
            "      },\n",
            atomic_load_explicit(&p->current_max_capacity, memory_order_acquire), leased, free_slots, marshalled,
            atomic_load_explicit(&p->allocated_groups_sz, memory_order_acquire));

        /* ── Groups / slots ──────────────────────────────────────────── */
        fputs("      \"groups\": [\n", s);
        for (int g = 0; g < atomic_load_explicit(&p->allocated_groups_sz, memory_order_acquire); g++) {
            AllocationGroupV2 *grp = p->allocation_groups[g];
            if (g > 0) fputs(",\n", s);

            /* Count free-stack depth */
            uint32_t fs_depth = 0;
            uintptr_t head = atomic_load(&grp->free_stack_head);
            while (head != 0) {
                fs_depth++;
                uint32_t idx = CDT_TreiberStackUnpackIdx(head);
                if (idx == 0) break;
                RecyclerV2PoolTypeEnvelop *env =
                    (RecyclerV2PoolTypeEnvelop *)((char *)grp->pool_memory
                        + idx * grp->stride);
                uint32_t next = atomic_load(&env->next_free);
                head = next ? CDT_TreiberStackPack(next,
                    CDT_TreiberStackUnpackPopCount(head)) : 0;
            }

            fprintf(s,
                "        {\n"
                "          \"groupid\": %u,\n"
                "          \"pool_size\": %zu,\n"
                "          \"free_stack_depth\": %u,\n"
                "          \"slots\": [\n",
                grp->groupid, grp->pool_size, fs_depth);

            size_t stride = sEnvelopeStrideV2(p->pool_handle.blocksz);
            for (size_t i = 0; i < grp->pool_size; i++) {
                RecyclerV2PoolTypeEnvelop *env =
                    (RecyclerV2PoolTypeEnvelop *)((char *)grp->pool_memory + i * stride);
                size_t rc = atomic_load(&env->_refcount);
                uintptr_t hw = atomic_load(&env->holder_word);
                bool refd = atomic_load(&env->referenced);
                uint64_t luts = atomic_load(&env->last_used_ts);

                if (i > 0) fputs(",\n", s);
                fprintf(s,
                    "            {"
                    "\"index\":%zu,"
                    "\"oid\":%zu,"
                    "\"refcount\":%zu,"
                    "\"state\":", i, env->oid, rc);
                sDumpJsonStringV2(s, sSlotStateLabelV2(rc, hw));
                fprintf(s,
                    ",\"holder_mode\":");
                sDumpJsonStringV2(s, sHolderModeLabelV2(hw));

                uint64_t mid = 0;
                if (hw != 0
                    && RecyclerV2IsMarshaller((InstanceHolderV2 *)hw)) {
                    mid = RecyclerV2MarshallerId(
                        atomic_load_explicit(&((InstanceHolderV2 *)hw)->holder.marshaller, memory_order_acquire));
                }
                fprintf(s,
                    ",\"marshaller_id\":%" PRIu64
                    ",\"referenced\":%s"
                    ",\"last_used_ts\":%" PRIu64 "}",
                    mid, refd ? "true" : "false", luts);
            }
            fputs("\n          ]\n        }", s);
        }
        fputs("\n      ],\n", s);

        /* ── Marshalled blobs on disk ────────────────────────────────── */
        fputs("      \"marshalled_blobs\": {\n", s);
        fputs("        \"storage_path\": ", s);
        sDumpJsonStringV2(s, p->storage_path);
        fprintf(s, ",\n        \"next_marshaller_id\": %" PRIu64 ",\n",
            atomic_load(&p->next_marshaller_id));
        fputs("        \"blobs\": ", s);
        sDumpStorageInventoryV2(s, p);
        fputs("\n      }\n", s);

        fputs("    }", s);
    }

    fprintf(s, "\n  ]\n}\n");
}

PUBLIC_API char *
RecyclerV2DumpStateToBuffer(void)
{
    char *buf = NULL;
    size_t sz  = 0;
    FILE *s = open_memstream(&buf, &sz);
    if (!s) return NULL;

    RecyclerV2DumpState(s);
    fclose(s);  // flushes and NUL-terminates buf

    return buf;
}

/* ── Describe (JSON, no json-c) ──────────────────────────────────────────── */

/*
 * Append @p str to the BufferDescriptor as a JSON string literal, escaping
 * the characters that would otherwise break the document.
 */
static void
sDescribeJsonString(BufferDescriptor *bd, const char *str)
{
    if (!str) {
        BufferDescriptorAppendFormatted(bd, "null");
        return;
    }
    BufferDescriptorAppendFormatted(bd, "\"");
    for (const char *p = str; *p; p++) {
        switch (*p) {
        case '"':  BufferDescriptorAppendFormatted(bd, "\\\""); break;
        case '\\': BufferDescriptorAppendFormatted(bd, "\\\\"); break;
        case '\n': BufferDescriptorAppendFormatted(bd, "\\n");  break;
        case '\t': BufferDescriptorAppendFormatted(bd, "\\t");  break;
        default:   BufferDescriptorAppendFormatted(bd, "%c", *p); break;
        }
    }
    BufferDescriptorAppendFormatted(bd, "\"");
}

static const char *
sDescribeStoragePolicyLabel(RecyclerV2StorageInitPolicy policy)
{
    switch (policy) {
    case RECYCLER_V2_STORAGE_OVERWRITE: return "OVERWRITE";
    case RECYCLER_V2_STORAGE_APPEND:    return "APPEND";
    case RECYCLER_V2_STORAGE_ARCHIVE:   return "ARCHIVE";
    default:                            return "UNKNOWN";
    }
}

/*
 * Count the free slots on a group's Treiber free stack without mutating it
 * (walk the intrusive next_free chain).
 */
static uint32_t
sDescribeFreeStackDepth(AllocationGroupV2 *group_ptr)
{
    uint32_t depth = 0;
    uintptr_t head = atomic_load_explicit(&group_ptr->free_stack_head,
                                          memory_order_acquire);
    while (head != 0) {
        depth++;
        uint32_t idx = CDT_TreiberStackUnpackIdx(head);
        if (idx == 0) break;
        RecyclerV2PoolTypeEnvelop *env =
            (RecyclerV2PoolTypeEnvelop *)((char *)group_ptr->pool_memory
                + idx * group_ptr->stride);
        uint32_t next = atomic_load_explicit(&env->next_free, memory_order_acquire);
        head = next ? CDT_TreiberStackPack(next, CDT_TreiberStackUnpackPopCount(head)) : 0;
    }
    return depth;
}

/*
 * Emit one pool as a JSON object.  The caller controls array separators;
 * this appends only the object body (no leading comma).
 */
static void
sDescribePool(BufferDescriptor *bd, RecyclerV2PoolDefinition *pool_ptr)
{
    size_t stride = sEnvelopeStrideV2(pool_ptr->pool_handle.blocksz);
    uint32_t groups = atomic_load_explicit(&pool_ptr->allocated_groups_sz,
                                           memory_order_acquire);

    BufferDescriptorAppendFormatted(bd,
        "\n    {\"type_index\":%u,\"type_name\":",
        pool_ptr->pool_handle.type);
    sDescribeJsonString(bd, pool_ptr->pool_handle.type_name);
    BufferDescriptorAppendFormatted(bd, ",\"blocksz\":%zu", pool_ptr->pool_handle.blocksz);
    BufferDescriptorAppendFormatted(bd, ",\"group_allocation_sz\":%u", pool_ptr->group_allocation_sz);
    BufferDescriptorAppendFormatted(bd, ",\"allocated_groups\":%u", groups);
    BufferDescriptorAppendFormatted(bd, ",\"expansion_threshold\":%u", pool_ptr->expansion_threshold);
    BufferDescriptorAppendFormatted(bd, ",\"marshal_watermark\":%.2f", (double)pool_ptr->marshal_watermark);
    BufferDescriptorAppendFormatted(bd, ",\"marshal_blob_max_sz\":%zu", pool_ptr->marshal_blob_max_sz);
    BufferDescriptorAppendFormatted(bd, ",\"storage_root\":");
    sDescribeJsonString(bd, pool_ptr->storage_root_ptr);
    BufferDescriptorAppendFormatted(bd, ",\"ufsrv_class\":");
    sDescribeJsonString(bd, pool_ptr->ufsrv_class_ptr);
    BufferDescriptorAppendFormatted(bd, ",\"instance_id\":");
    sDescribeJsonString(bd, pool_ptr->instance_id_ptr);
    /* Whether this pool was configured to report.  Emitted beside the other
       caller-supplied deployment context, because "no diagnostics appeared" and
       "no logger was configured" call for different fixes. */
    BufferDescriptorAppendFormatted(bd, ",\"logger\":\"%s\"",
        pool_ptr->uf_logger != NULL ? "enabled" : "none");
    BufferDescriptorAppendFormatted(bd, ",\"storage_path\":");
    sDescribeJsonString(bd, pool_ptr->storage_path);
    BufferDescriptorAppendFormatted(bd, ",\"storage_init_policy\":\"%s\"",
        sDescribeStoragePolicyLabel(pool_ptr->storage_init_policy));
    BufferDescriptorAppendFormatted(bd, ",\"next_marshaller_id\":%" PRIu64,
        atomic_load_explicit(&pool_ptr->next_marshaller_id, memory_order_acquire));
    BufferDescriptorAppendFormatted(bd, ",\"expanding\":%s",
        atomic_load_explicit(&pool_ptr->expanding, memory_order_acquire) ? "true" : "false");
    BufferDescriptorAppendFormatted(bd, ",\"clock_hand\":%" PRIu64,
        atomic_load_explicit(&pool_ptr->clock_hand, memory_order_acquire));

    /* Per-group occupancy + aggregate capacity accounting. */
    size_t leased = 0, free_slots = 0, marshalled = 0;
    BufferDescriptorAppendFormatted(bd, ",\"groups\":[");
    for (uint32_t g = 0; g < groups; g++) {
        AllocationGroupV2 *grp = pool_ptr->allocation_groups[g];
        size_t g_leased = 0, g_free = 0, g_marshalled = 0;
        for (size_t i = 0; i < grp->pool_size; i++) {
            RecyclerV2PoolTypeEnvelop *env =
                (RecyclerV2PoolTypeEnvelop *)((char *)grp->pool_memory + i * stride);
            size_t rc = atomic_load_explicit(&env->_refcount, memory_order_acquire);
            if (rc >= 2) {
                g_leased++; leased++;
            } else if (rc == 1) {
                uintptr_t hw = atomic_load_explicit(&env->holder_word, memory_order_acquire);
                if (hw != 0) { g_marshalled++; marshalled++; }
                else         { g_free++; free_slots++; }
            }
        }
        BufferDescriptorAppendFormatted(bd,
            "%s{\"groupid\":%u,\"pool_size\":%zu,\"free_stack_depth\":%u,"
            "\"leased\":%zu,\"free\":%zu,\"marshalled\":%zu}",
            g == 0 ? "" : ",", grp->groupid, grp->pool_size,
            sDescribeFreeStackDepth(grp), g_leased, g_free, g_marshalled);
    }
    BufferDescriptorAppendFormatted(bd, "],\"capacity\":{\"total\":%zu,"
        "\"leased\":%zu,\"free\":%zu,\"marshalled\":%zu}}",
        atomic_load_explicit(&pool_ptr->current_max_capacity, memory_order_acquire),
        leased, free_slots, marshalled);
}

PUBLIC_API BufferDescriptor *
DescribeRecycler(const char *type_pool_name, BufferDescriptor *provided)
{
    if (!provided) {
        provided = calloc(1, sizeof(BufferDescriptor));
        if (!provided) return NULL;
        BufferDescriptorInit(provided, 256);
    }

    BufferDescriptorAppendFormatted(provided, "{\"count_types\":%u,\"pools\":[",
        sRecyclerV2Ptr->count_types);

    bool emitted = false;
    for (uint16_t t = 0; t < sRecyclerV2Ptr->count_types; t++) {
        RecyclerV2PoolDefinition *pool_ptr = sRecyclerV2Ptr->pools_ptr[t];
        if (IS_EMPTY_V2(pool_ptr)) continue;
        if (type_pool_name &&
            strcmp(pool_ptr->pool_handle.type_name, type_pool_name) != 0) {
            continue;
        }
        if (emitted) BufferDescriptorAppendFormatted(provided, ",");
        sDescribePool(provided, pool_ptr);
        emitted = true;
    }

    BufferDescriptorAppendFormatted(provided, "\n]}\n");
    return provided;
}

PUBLIC_API CollectionDescriptor *
ListRecyclerTypes(void)
{
    size_t n = 0;
    size_t str_bytes = 0;

    /* Pass 1 — count registered (non-empty) pools and total name bytes. */
    for (uint16_t t = 0; t < sRecyclerV2Ptr->count_types; t++) {
        RecyclerV2PoolDefinition *pool_ptr = sRecyclerV2Ptr->pools_ptr[t];
        if (IS_EMPTY_V2(pool_ptr) || !pool_ptr->pool_handle.type_name) continue;
        n++;
        str_bytes += strlen(pool_ptr->pool_handle.type_name) + 1;
    }

    /* One contiguous slab: struct + pointer array + name strings. */
    size_t arr_bytes = n * sizeof(collection_t *);
    size_t total = sizeof(CollectionDescriptor) + arr_bytes + str_bytes;
    char *block = malloc(total);
    if (!block) return NULL;

    CollectionDescriptor *cd = (CollectionDescriptor *)block;
    cd->collection = (n > 0) ? (collection_t **)(block + sizeof(CollectionDescriptor)) : NULL;
    cd->collection_sz = n;
    cd->collection_base_offset = 0;

    /* Pass 2 — copy the type names into the same slab. */
    char *str = block + sizeof(CollectionDescriptor) + arr_bytes;
    size_t i = 0;
    for (uint16_t t = 0; t < sRecyclerV2Ptr->count_types && i < n; t++) {
        RecyclerV2PoolDefinition *pool_ptr = sRecyclerV2Ptr->pools_ptr[t];
        if (IS_EMPTY_V2(pool_ptr) || !pool_ptr->pool_handle.type_name) continue;
        size_t len = strlen(pool_ptr->pool_handle.type_name) + 1;
        memcpy(str, pool_ptr->pool_handle.type_name, len);
        cd->collection[i++] = (collection_t *)str;
        str += len;
    }

    return cd;
}
