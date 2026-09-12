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
 * @file instance_holder_v2_type.h
 * @brief InstanceHolderV2 — tagged-pointer wrapper for stable object references
 *
 * InstanceHolderV2 is a tagged-pointer union that provides a stable reference
 * to a pool-allocated object.  It has two modes:
 *
 *   Instance mode (bit[0] == 0):
 *     [63:3]  aligned pointer to ClientContextData (61 usable bits)
 *     [2:1]   tag (2 bits, DEFAULT_TAG = 1)
 *     [0]     0 = Instance
 *
 *   Marshaller mode (bit[0] == 1):
 *     [63:48] type_index   (16 bits → 65535 max types)
 *     [47:1]  marshaller_id (47 bits → 140 trillion IDs per type)
 *     [0]     1 = Marshaller
 *
 * The recycler returns a heap-allocated InstanceHolderV2 from RecyclerV2Get()
 * and RecyclerV2GetNewInstance(); the holder is caller-owned (free it after
 * RecyclerV2Put()/RecyclerV2DestroyInstance()).  Consumers reference it by
 * pointer (typically via a typedef for the concrete type) and use the inline
 * accessor functions.  The slow-path resolution for marshalled holders is
 * implemented in recycler_v2.c.
 */

#ifndef UFLIB_RECYCLER_V2_INSTANCE_HOLDER_V2_TYPE_H
#define UFLIB_RECYCLER_V2_INSTANCE_HOLDER_V2_TYPE_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <assert.h>

#include <uflib/main_types.h>
#include <uflib/recycler_v2/recycler_v2_defs.h>

/* ── Marshaller context type ──────────────────────────────────────────────── */

/** Opaque marshaller identifier — consumer-defined, recycler-transparent. */
typedef uintptr_t MarshallerContextData;

/* ── Bit layout constants ────────────────────────────────────────────────── */

#define RECYCLER_V2_INSTANCE_MASK   (~((uintptr_t)0x7ULL))   ///< [63:3] — strips low 3 bits
#define RECYCLER_V2_TAG_MASK        ((uintptr_t)0x6ULL)       ///< [2:1]  — 2-bit tag field
#define RECYCLER_V2_MODE_MASK       ((uintptr_t)0x1ULL)       ///< [0]    — 0=Instance, 1=Marshaller

#define RECYCLER_V2_TAG_SHIFT       1                          ///< tag resides at bits [2:1]
#define RECYCLER_V2_DEFAULT_TAG     PRIV_CONFIG_DEFAULT_RECYCLER_V2_INSTANCE_TAG

#define RECYCLER_V2_MARSHALLER_TYPE_SHIFT   48                 ///< type_index at [63:48]
#define RECYCLER_V2_MARSHALLER_ID_SHIFT     1                  ///< marshaller_id at [47:1]
#define RECYCLER_V2_MARSHALLER_ID_MASK      0x7FFFFFFFFFFFULL ///< 47-bit mask

/* ── InstanceHolderV2 type ────────────────────────────────────────────────── */

/*! Tagged-pointer wrapper — stable object reference for consumers.
 *
 *  The recycler returns a heap-allocated InstanceHolderV2 (caller-owned); the
 *  holder word itself is atomic, so concurrent resolver/marshaller transitions
 *  are defined by C17.  Consumers reference the holder by pointer, typically
 *  via a typedef for the concrete type:
 *  @code{.c}
 *  typedef InstanceHolderV2 InstanceHolderForSessionV2;
 *  @endcode
 */
typedef struct InstanceHolderV2 {
    union {
        _Atomic uintptr_t marshaller;      ///< atomic tagged holder word
        ClientContextData *instance;       ///< compatibility alias; use accessors
    } holder;
} InstanceHolderV2;

/* ── Mode detection (inline) ──────────────────────────────────────────────── */

/**
 * @brief Test whether the holder is in Instance mode (object pointer valid).
 * @param ih_ptr  InstanceHolderV2 to test.
 * @return true if bit[0] == 0 (Instance mode).
 */
static inline bool
RecyclerV2IsInstance(InstanceHolderV2 *ih_ptr)
{
    return (atomic_load_explicit(&ih_ptr->holder.marshaller, memory_order_acquire) & RECYCLER_V2_MODE_MASK) == 0;
}

/**
 * @brief Test whether the holder is in Marshaller mode (object serialized).
 * @param ih_ptr  InstanceHolderV2 to test.
 * @return true if bit[0] == 1 (Marshaller mode).
 */
static inline bool
RecyclerV2IsMarshaller(InstanceHolderV2 *ih_ptr)
{
    return (atomic_load_explicit(&ih_ptr->holder.marshaller, memory_order_acquire) & RECYCLER_V2_MODE_MASK) != 0;
}

/* ── Tag extraction (inline) ──────────────────────────────────────────────── */

/**
 * @brief Extract the 2-bit tag from an Instance-mode holder.
 * @param ih_ptr  InstanceHolderV2 in Instance mode.
 * @return Tag value (0–1).
 */
static inline uint8_t
RecyclerV2GetTag(InstanceHolderV2 *ih_ptr)
{
    return (uint8_t)((atomic_load_explicit(&ih_ptr->holder.marshaller, memory_order_acquire) & RECYCLER_V2_TAG_MASK) >> RECYCLER_V2_TAG_SHIFT);
}

/* ── Instance mode setters (inline) ───────────────────────────────────────── */

/**
 * @brief Assign an object pointer with an explicit tag value.
 *
 * The pointer MUST be aligned to RECYCLER_V2_INSTANCE_BYTE_ALIGNMENT (8 bytes).
 * The tag MUST be <= RECYCLER_V2_INSTANCE_MAX_TAG (1).
 *
 * @param ih_ptr       Pre-allocated InstanceHolderV2.
 * @param instance_ptr  Aligned object pointer to assign.
 * @param tag           2-bit max tag value.
 */
static inline void
RecyclerV2SetInstanceWithTag(InstanceHolderV2 *ih_ptr, void *instance_ptr, uint8_t tag)
{
    assert(((uintptr_t)(instance_ptr) & RECYCLER_V2_TAG_MASK) == 0);
    assert(((tag << RECYCLER_V2_TAG_SHIFT) & RECYCLER_V2_INSTANCE_MASK) == 0);

    atomic_store_explicit(&ih_ptr->holder.marshaller, (uintptr_t)instance_ptr | (tag << RECYCLER_V2_TAG_SHIFT), memory_order_release);
}

/**
 * @brief Assign an object pointer with the default tag.
 * @param ih_ptr       Pre-allocated InstanceHolderV2.
 * @param instance_ptr  Aligned object pointer to assign.
 */
static inline void
RecyclerV2SetInstance(InstanceHolderV2 *ih_ptr, void *instance_ptr)
{
    RecyclerV2SetInstanceWithTag(ih_ptr, instance_ptr, RECYCLER_V2_DEFAULT_TAG);
}

/* ── Marshaller mode setters (inline) ─────────────────────────────────────── */

/**
 * @brief Assign a marshaller ID to the holder, putting it in Marshaller mode.
 *
 * The holder is now "hijacked" — the object pointer is no longer valid.
 * The consumer must call RecyclerV2GetInstance() to re-materialize the object.
 *
 * @param ih_ptr  Pre-allocated InstanceHolderV2.
 * @param id       Consumer-defined marshaller identifier.
 */
static inline void
RecyclerV2SetMarshaller(InstanceHolderV2 *ih_ptr, uintptr_t id)
{
    atomic_store_explicit(&ih_ptr->holder.marshaller, (id << RECYCLER_V2_MARSHALLER_ID_SHIFT) | 1, memory_order_release);
}

/* ── Marshaller mode getter (inline) ──────────────────────────────────────── */

/**
 * @brief Extract the marshaller ID from a Marshaller-mode holder.
 * @param ih_ptr  InstanceHolderV2 in Marshaller mode.
 * @return Consumer-defined marshaller identifier.
 */
static inline uintptr_t
RecyclerV2GetMarshaller(InstanceHolderV2 *ih_ptr)
{
    return atomic_load_explicit(&ih_ptr->holder.marshaller, memory_order_acquire) >> RECYCLER_V2_MARSHALLER_ID_SHIFT;
}

/* ── Marshaller encoding helpers (inline) ─────────────────────────────────── */

/**
 * @brief Extract the type_index from a V2 marshaller-encoded word.
 * @param word  Full uintptr_t marshaller encoding (bit[0] == 1).
 * @return Type index (1..65535).
 */
static inline uint16_t
RecyclerV2MarshallerTypeIndex(uintptr_t word)
{
    return (uint16_t)(word >> RECYCLER_V2_MARSHALLER_TYPE_SHIFT);
}

/**
 * @brief Extract the marshaller_id from a V2 marshaller-encoded word.
 * @param word  Full uintptr_t marshaller encoding (bit[0] == 1).
 * @return Marshaller ID (47-bit).
 */
static inline uint64_t
RecyclerV2MarshallerId(uintptr_t word)
{
    return (word >> RECYCLER_V2_MARSHALLER_ID_SHIFT) & RECYCLER_V2_MARSHALLER_ID_MASK;
}

/**
 * @brief Encode a type_index and marshaller_id into a V2 marshaller word.
 * @param type_index     Type index (1..65535).
 * @param marshaller_id  Marshaller ID (47-bit).
 * @return Encoded uintptr_t with bit[0] == 1.
 */
static inline uintptr_t
RecyclerV2EncodeMarshaller(uint16_t type_index, uint64_t marshaller_id)
{
    assert(type_index != 0);
    assert((marshaller_id & ~RECYCLER_V2_MARSHALLER_ID_MASK) == 0);
    return ((uintptr_t)type_index << RECYCLER_V2_MARSHALLER_TYPE_SHIFT)
         | ((marshaller_id & RECYCLER_V2_MARSHALLER_ID_MASK) << RECYCLER_V2_MARSHALLER_ID_SHIFT)
         | 1;
}

/* ── Object resolution (inline fast path + out-of-line slow path) ─────────── */

/* Forward declaration — implemented in recycler_v2.c */
void *RecyclerV2ResolveMarshalledInstance(InstanceHolderV2 *ih_ptr);
void  RecyclerV2EnqueuePendingUnmarshal(InstanceHolderV2 *ih_ptr);

/**
 * @brief Obtain the object pointer from an InstanceHolderV2.
 *
 * Fast path (Instance mode): strips low bits and returns the pointer directly.
 * Slow path (Marshaller mode): delegates to RecyclerV2ResolveMarshalledInstance()
 * which pops a free object, calls the unmarshal callback, CASes the holder
 * word to Instance mode, and returns the materialized object.
 *
 * This is the central resolution point — all type-provider wrappers call this.
 *
 * @param ih_ptr  InstanceHolderV2.
 * @return Object pointer (ClientContextData *), or NULL if resolution fails.
 *
 * @code{.c}
 * static inline Session *
 * SessionOffInstanceHolderV2(InstanceHolderForSessionV2 *ih_ptr) {
 *     return (Session *)RecyclerV2GetInstance(ih_ptr);
 * }
 * @endcode
 */
static inline void *
RecyclerV2GetInstance(InstanceHolderV2 *ih_ptr)
{
    uintptr_t word = atomic_load_explicit(&ih_ptr->holder.marshaller, memory_order_acquire);
    if ((word & RECYCLER_V2_MODE_MASK) == 0) {
        /* Fast path — Instance mode: strip low 3 bits, return pointer */
        return (void *)(word & RECYCLER_V2_INSTANCE_MASK);
    }
    /* Slow path — Marshaller mode: out-of-line resolution in recycler_v2.c */
    return RecyclerV2ResolveMarshalledInstance(ih_ptr);
}

/**
 * @brief Non-blocking variant of RecyclerV2GetInstance().
 *
 * Fast path (Instance mode): same as RecyclerV2GetInstance() — returns the
 * pointer directly.
 * Slow path (Marshaller mode): enqueues the holder on the pending unmarshal
 * stack and returns NULL.  The pending queue is drained opportunistically
 * on RecyclerV2Put().  The consumer should retry on the next event loop
 * iteration.
 *
 * @param ih_ptr  InstanceHolderV2.
 * @return Object pointer, or NULL if the holder is in Marshaller mode.
 */
static inline void *
RecyclerV2GetInstanceTry(InstanceHolderV2 *ih_ptr)
{
    uintptr_t word = atomic_load_explicit(&ih_ptr->holder.marshaller, memory_order_acquire);
    if ((word & RECYCLER_V2_MODE_MASK) == 0) {
        return (void *)(word & RECYCLER_V2_INSTANCE_MASK);
    }
    RecyclerV2EnqueuePendingUnmarshal(ih_ptr);
    return NULL;
}

#endif /* UFLIB_RECYCLER_V2_INSTANCE_HOLDER_V2_TYPE_H */
