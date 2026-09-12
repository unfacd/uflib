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
 * @file cdt_treiber_stack.h
 * @brief Shared ABA-protected Treiber stack for lock-free free-list management
 *
 * This is the canonical Treiber stack implementation extracted from
 * LocklessFixedWidthHashMap (src/cdt/hashmap/cdt_fixed_width_hashmap.c).
 * It provides the packing/unpacking helpers and the push/pop operations,
 * parameterized so any module can embed a free stack without duplicating
 * the ABA-protection logic.
 *
 * ## Encoding
 *
 *     [63:32]  pop_count   — incremented on every push AND every pop
 *     [31:0]   pool_index  — index into the module's pool array
 *
 * ## Sentinel
 *
 * Index 0 is reserved as the EMPTY sentinel.  `head == 0` unambiguously
 * means "stack empty."  The caller must ensure that pool index 0 is never
 * allocated — size the pool one larger than the logical capacity, and
 * never push index 0 onto the stack.
 *
 * ## Required fields in each pool node
 *
 * Each node MUST contain a field `_Atomic uint32_t next_free` that the
 * stack uses for its intrusive linked list.  The caller provides the
 * byte offset of this field via @ref CDT_TREIBER_NEXT_FREE_OFFSET.
 *
 * ## Usage
 *
 * @code{.c}
 * // In your module's node struct:
 * typedef struct {
 *     _Atomic uint32_t next_free;  // REQUIRED — at a known offset
 *     // ... other fields ...
 * } MyNode;
 *
 * // In your module's container struct:
 * typedef struct {
 *     _Atomic uintptr_t free_stack_head;  // Treiber stack head
 *     void             *pool;             // contiguous node array
 *     size_t            node_stride;      // byte stride between nodes
 *     uint32_t          pool_capacity;    // total nodes (incl. sentinel index 0)
 * } MyModule;
 *
 * // Pack/unpack helpers:
 * #define MY_TREIBER_NEXT_FREE_OFFSET  offsetof(MyNode, next_free)
 *
 * // Init:
 * atomic_init(&mod->free_stack_head, 0);
 * for (uint32_t i = mod->pool_capacity - 1; i > 0; i--) {
 *     CDT_TreiberStackPush(&mod->free_stack_head, mod->pool,
 *                          mod->node_stride, MY_TREIBER_NEXT_FREE_OFFSET, i);
 * }
 *
 * // Pop (returns 0 if empty):
 * uint32_t idx = CDT_TreiberStackPop(&mod->free_stack_head, mod->pool,
 *                                    mod->node_stride, MY_TREIBER_NEXT_FREE_OFFSET);
 *
 * // Push:
 * CDT_TreiberStackPush(&mod->free_stack_head, mod->pool,
 *                      mod->node_stride, MY_TREIBER_NEXT_FREE_OFFSET, idx);
 * @endcode
 *
 * @note This header is usable from both C11 and C++17 (via the uflib
 *       C11/C++17 atomics bridge in cdt_spinlock_defs.h or direct
 *       stdatomic.h includes).
 */

#ifndef UFLIB_CDT_CDT_TREIBER_STACK_H
#define UFLIB_CDT_CDT_TREIBER_STACK_H

#include <stddef.h>
#include <stdint.h>

/* The caller must have <stdatomic.h> (C11) or <atomic> + the bridge
 * (C++17) included before this header.  We only need the atomics types
 * and memory_order constants. */
#ifndef __STDC_NO_ATOMICS__
#include <stdatomic.h>
#endif

#ifdef __cplusplus
#include <atomic>
#endif

/* ── Packing / unpacking helpers ──────────────────────────────────────────── */

/**
 * @brief Pack a pool index and pop count into a tagged stack-head word.
 *
 * Layout: [63:32] pop_count, [31:0] pool_index.
 *
 * @param idx         Pool index (1..capacity-1; 0 is reserved sentinel).
 * @param pop_count   Monotonic counter (incremented on every push and pop).
 * @return Tagged uintptr_t for CAS.
 */
static inline uintptr_t
CDT_TreiberStackPack(uint32_t idx, uint64_t pop_count)
{
    return ((uintptr_t)pop_count << 32) | idx;
}

/**
 * @brief Extract the pool index from a tagged stack-head word.
 * @param head  Tagged uintptr_t from free_stack_head.
 * @return Pool index (lower 32 bits).
 */
static inline uint32_t
CDT_TreiberStackUnpackIdx(uintptr_t head)
{
    return (uint32_t)(head & 0xFFFFFFFFU);
}

/**
 * @brief Extract the pop count from a tagged stack-head word.
 * @param head  Tagged uintptr_t from free_stack_head.
 * @return Pop count (upper 32 bits).
 */
static inline uint64_t
CDT_TreiberStackUnpackPopCount(uintptr_t head)
{
    return (uint64_t)(head >> 32);
}

/* ── Core operations ──────────────────────────────────────────────────────── */

/**
 * @brief Pop a free node index from the Treiber stack (lock-free).
 *
 * ABA protection: the pop_count in bits [63:32] is incremented on every
 * push AND every pop.  Even if the same pool index is recycled, the
 * counter differs, so the CAS fails and the thread retries.
 *
 * @param head_ptr            Pointer to _Atomic uintptr_t free_stack_head.
 * @param pool_base           Base address of the contiguous pool array.
 * @param node_stride         Byte stride between consecutive pool nodes.
 * @param next_free_offset    Byte offset of the _Atomic uint32_t next_free
 *                            field within each pool node.
 * @return Pool index of the popped node, or 0 if the stack is empty
 *         (index 0 is the reserved EMPTY sentinel — never allocated).
 */
static inline uint32_t
CDT_TreiberStackPop(_Atomic uintptr_t *head_ptr,
                    void *pool_base,
                    size_t node_stride,
                    size_t next_free_offset)
{
    uintptr_t head = atomic_load_explicit(head_ptr, memory_order_acquire);

    while (head != 0) {
        uint32_t idx = CDT_TreiberStackUnpackIdx(head);

        /* Derive the node and read its next_free link */
        char *node = (char *)pool_base + (size_t)idx * node_stride;
        _Atomic uint32_t *next_free_ptr =
            (_Atomic uint32_t *)(node + next_free_offset);
        uint32_t next_idx =
            atomic_load_explicit(next_free_ptr, memory_order_relaxed);

        uintptr_t new_head = CDT_TreiberStackPack(
            next_idx, CDT_TreiberStackUnpackPopCount(head) + 1);

        if (atomic_compare_exchange_strong_explicit(
                head_ptr, &head, new_head,
                memory_order_acq_rel, memory_order_relaxed)) {
            return idx;
        }
    }
    return 0; /* Stack empty (index 0 = sentinel, never allocated) */
}

/**
 * @brief Push a free node index onto the Treiber stack (lock-free).
 *
 * ABA protection: the pop_count is incremented on every push (not just
 * on pop).  This ensures that a rapid free-allocate-free cycle cannot
 * reproduce the same (idx, count) pair.
 *
 * @param head_ptr            Pointer to _Atomic uintptr_t free_stack_head.
 * @param pool_base           Base address of the contiguous pool array.
 * @param node_stride         Byte stride between consecutive pool nodes.
 * @param next_free_offset    Byte offset of the _Atomic uint32_t next_free
 *                            field within each pool node.
 * @param idx                 Pool index to push (must be >= 1).
 */
static inline void
CDT_TreiberStackPush(_Atomic uintptr_t *head_ptr,
                     void *pool_base,
                     size_t node_stride,
                     size_t next_free_offset,
                     uint32_t idx)
{
    char *node = (char *)pool_base + (size_t)idx * node_stride;
    _Atomic uint32_t *next_free_ptr =
        (_Atomic uint32_t *)(node + next_free_offset);

    uintptr_t old_head =
        atomic_load_explicit(head_ptr, memory_order_acquire);
    uintptr_t new_head;
    do {
        atomic_store_explicit(next_free_ptr,
            CDT_TreiberStackUnpackIdx(old_head), memory_order_relaxed);
        new_head = CDT_TreiberStackPack(
            idx, CDT_TreiberStackUnpackPopCount(old_head) + 1);
    } while (!atomic_compare_exchange_weak_explicit(
        head_ptr, &old_head, new_head,
        memory_order_release, memory_order_relaxed));
}

#endif /* UFLIB_CDT_CDT_TREIBER_STACK_H */
