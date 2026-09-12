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
 * @file recycler_v2.h
 * @brief RecyclerV2 — Lock-free slab allocator with marshaller-based object lifecycle
 *
 * RecyclerV2 provides pre-allocated, type-pooled object storage with:
 *   - Lock-free Get/Put via tagged-pointer Treiber stack
 *   - Seqlock-protected group expansion
 *   - Atomic single-holder word (CAS) + optional fallback multi-holder list
 *   - CLOCK second-chance marshaller: serializes idle objects to disk under
 *     memory pressure, re-materializes on-demand
 *   - Configurable soft watermark per type pool
 *   - Transparent bootstrap via RecyclerV2GetInstance() slow path
 *
 * V1 and V2 coexist in libuflib.a.  V2 is an additive module — V1 consumers
 * continue to compile and run unchanged.  See the V1→V2 transition guide for
 * per-type incremental migration steps.
 */

#ifndef UFLIB_RECYCLER_V2_RECYCLER_V2_H
#define UFLIB_RECYCLER_V2_RECYCLER_V2_H

#include <uflib/uflib_defs.h>

#include <stdio.h>
#include <stdatomic.h>
#include <uflib/recycler_v2/instance_holder_v2_type.h>
#include <uflib/recycler_v2/recycler_v2_type.h>
#include <uflib/recycler_v2/recycler_v2_defs.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/main_types.h>

/* ── Return codes ─────────────────────────────────────────────────────────── */

#define RECYCLER_V2_INSTANCE_HOLDER_FOUND      1
#define RECYCLER_V2_INSTANCE_HOLDER_NOT_FOUND  0

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/**
 * @brief Register a new type pool with the recycler and allocate the first
 *        allocation group.
 *
 * All configuration fields have sensible defaults — see RecyclerV2PoolConfig.
 * The returned handle is valid for the lifetime of the process.
 *
 * @param config_ptr  Pre-filled config struct (NULL uses all defaults).
 * @return Opaque handle, or NULL on failure.
 *
 * @code{.c}
 * RecyclerV2PoolConfig cfg = {
 *     .type_name            = "Session",
 *     .blocksz              = sizeof(Session),
 *     .group_allocation_sz  = 16,
 *     .expansion_threshold  = 1024,
 *     .ops_ptr              = &session_ops,
 *     .marshal_watermark    = 0.8f,
 *     .marshal_blob_max_sz  = 4096,  // consumer declares max serialized size
 * };
 * RecyclerV2PoolHandle *h = RecyclerV2InitTypePool(&cfg);
 * if (!h) { // handle error }
 * @endcode
 */
PUBLIC_API RecyclerV2PoolHandle *
RecyclerV2InitTypePool(const RecyclerV2PoolConfig *config_ptr);

/* ── Object operations ────────────────────────────────────────────────────── */

/**
 * @brief Obtain an object from the pool.
 *
 * If the soft marshal_watermark is breached, this call may inline-trigger a
 * CLOCK victim scan and marshal an idle object to free a slot.  If no free
 * slot exists and no victim is found, triggers group expansion (up to
 * group_allocation_sz).  Returns NULL only on hard exhaustion.
 *
 * @param handle_ptr    Pool handle from RecyclerV2InitTypePool().
 * @param context_data  Opaque context passed to initget callback.
 * @param call_flags    Type-specific flags passed to initget callback.
 * @return InstanceHolderV2 *, or NULL if pool is exhausted.
 *
 * @note Ownership: the returned holder is heap-allocated and owned by the
 *       caller.  Free it with free() after RecyclerV2Put() or
 *       RecyclerV2DestroyInstance() — the recycler does not free it.
 */
PUBLIC_API InstanceHolderV2 *
RecyclerV2Get(RecyclerV2PoolHandle *handle_ptr, ContextData *context_data,
              unsigned long call_flags);

/**
 * @brief Return an object to the pool.
 *
 * Opportunistically drains the pending unmarshal queue after enqueuing.
 * The InstanceHolderV2 must be in Instance mode — marshalled holders
 * are rejected (return -5).
 *
 * @param ih_ptr       InstanceHolderV2 obtained from RecyclerV2Get().
 * @param context_data  Opaque context passed to initput callback.
 * @param call_flags    Type-specific flags.
 *
 * @return 0 on success, negative error code on failure.
 *
 * @note Ownership: the recycler does not free @p ih_ptr.  The caller retains
 *       ownership of the holder and must free() it after this call returns.
 */
PUBLIC_API int
RecyclerV2Put(InstanceHolderV2 *ih_ptr, ContextData *context_data,
              unsigned long call_flags);

/**
 * @brief Create an additional InstanceHolderV2 for an already-leased object.
 *
 * This is the multi-holder path — the object can be shared across consumers.
 * Triggers fallback list allocation (cold path, pthread_spinlock_t).
 * Currently unused by any downstream consumer.
 *
 * @param ih_ptr  Existing InstanceHolderV2 (must be in Instance mode).
 * @return New InstanceHolderV2 referencing the same object, or NULL on failure.
 *
 * @note Ownership: the returned holder is heap-allocated and owned by the
 *       caller.  Free it with free() after RecyclerV2DestroyInstance() or
 *       RecyclerV2Put() — the recycler does not free it.
 */
PUBLIC_API InstanceHolderV2 *
RecyclerV2GetNewInstance(InstanceHolderV2 *ih_ptr);

/**
 * @brief Destroy an InstanceHolderV2, removing its reference to the object.
 *
 * Does NOT return the object to the pool — use RecyclerV2Put() for that.
 *
 * @param ih_ptr  InstanceHolderV2 to destroy.
 *
 * @return RECYCLER_V2_INSTANCE_HOLDER_FOUND (1) or
 *         RECYCLER_V2_INSTANCE_HOLDER_NOT_FOUND (0).
 *
 * @note Ownership: the recycler does not free @p ih_ptr.  The caller retains
 *       ownership of the holder and must free() it after this call returns.
 */
PUBLIC_API int
RecyclerV2DestroyInstance(InstanceHolderV2 *ih_ptr);

/* ── Reference counting ──────────────────────────────────────────────────── */

/**
 * @brief Increment the reference count on an object.
 *
 * The type is derived from the InstanceHolderV2 encoding — no explicit
 * type parameter is needed (unlike V1).
 *
 * @param ih_ptr    InstanceHolderV2.
 * @param multiples  Number of references to add.
 */
PUBLIC_API void
RecyclerV2Referenced(InstanceHolderV2 *ih_ptr, int multiples);

/**
 * @brief Decrement the reference count on an object.
 *
 * @param ih_ptr    InstanceHolderV2.
 * @param multiples  Number of references to remove.
 */
PUBLIC_API void
RecyclerV2UnReferenced(InstanceHolderV2 *ih_ptr, int multiples);

/**
 * @brief Query the current reference count.
 *
 * @param ih_ptr  InstanceHolderV2.
 * @return Current refcount value.
 */
PUBLIC_API size_t
RecyclerV2GetReferenceCount(InstanceHolderV2 *ih_ptr);

/* ── Client context extraction ────────────────────────────────────────────── */

/**
 * @brief Extract the recycler-managed object pointer from an InstanceHolderV2.
 *
 * Convenience wrapper — equivalent to RecyclerV2GetInstance(ih_ptr).
 *
 * @param ih_ptr  InstanceHolderV2.
 * @return Object pointer (ClientContextData *).
 */
PUBLIC_API ClientContextData *
RecyclerV2GetClientContextData(InstanceHolderV2 *ih_ptr);

/**
 * @brief Find the InstanceHolderV2 wrapping a given object pointer.
 *
 * Searches the holder_word / fallback list for the holder whose instance
 * matches ctx_ptr.
 *
 * @param ctx_ptr  Object pointer to search for.
 * @return InstanceHolderV2 *, or NULL if not found.
 */
PUBLIC_API InstanceHolderV2 *
RecyclerV2InstanceHolderFromClientContext(ContextData *ctx_ptr);

/* ── Marshaller configuration ─────────────────────────────────────────────── */

/**
 * @brief Set the marshal watermark for a type pool.
 *
 * When (total_leased / current_max_capacity) >= watermark, RecyclerV2Get()
 * will attempt to marshal idle objects before triggering expansion.
 *
 * @param handle_ptr  Pool handle.
 * @param watermark   0.0–1.0 fraction.  1.0 = hard exhaustion only (default).
 */
PUBLIC_API void
RecyclerV2SetMarshalWatermark(RecyclerV2PoolHandle *handle_ptr, float watermark);

/* ── Query functions ──────────────────────────────────────────────────────── */

/**
 * @brief Return the total number of objects in a type pool (leased + free).
 *
 * @param handle_ptr  Pool handle.
 * @return Current maximum capacity across all allocation groups.
 */
PUBLIC_API size_t
RecyclerV2GetCapacity(RecyclerV2PoolHandle *handle_ptr);

/**
 * @brief Return the number of currently leased objects.
 *
 * @param handle_ptr  Pool handle.
 * @return Number of objects currently in use (refcount >= 2).
 */
PUBLIC_API size_t
RecyclerV2GetLeasedCount(RecyclerV2PoolHandle *handle_ptr);

/**
 * @brief Return the effective pool configuration as resolved at init time.
 *
 * Fills `out_ptr` with the actual values in use — user-supplied where
 * provided, compile-time defaults where the user passed 0/NULL.
 * `out_ptr->type_name` and `out_ptr->storage_root` point to the
 * recycler-owned strings (do not free).  `out_ptr->ops_ptr` points
 * to the recycler-owned ops copy.
 *
 * @param handle_ptr  Pool handle from RecyclerV2InitTypePool().
 * @param out_ptr     Pre-allocated config struct to populate.
 *
 * @code{.c}
 * RecyclerV2PoolConfig cfg;
 * RecyclerV2GetPoolConfig(handle, &cfg);
 * printf("type=%s blocksz=%zu groups=%u threshold=%u storage=%s\n",
 *     cfg.type_name, cfg.blocksz, cfg.group_allocation_sz,
 *     cfg.expansion_threshold, cfg.storage_root);
 * @endcode
 */
PUBLIC_API void
RecyclerV2GetPoolConfig(RecyclerV2PoolHandle *handle_ptr,
                        RecyclerV2PoolConfig *out_ptr);

/* ── Introspection / debugging ──────────────────────────────────────────────── */

/**
 * @brief Dump the full recycler state as JSON to a stream.
 *
 * Writes a complete snapshot including all pools, groups, slot-level
 * status (free / leased / marshalled), config, and an inventory of
 * marshalled blobs on disk.  This is a debug/introspection API —
 * the output may be large for production pools.
 *
 * @param stream  Open FILE stream (e.g. stdout, a fdopen'd fd).
 *
 * @code{.c}
 * FILE *f = fopen("/tmp/recycler_state.json", "w");
 * RecyclerV2DumpState(f);
 * fclose(f);
 * @endcode
 */
PUBLIC_API void
RecyclerV2DumpState(FILE *stream);

/**
 * @brief Dump the full recycler state as a JSON string in memory.
 *
 * Returns a malloc'd, null-terminated string containing the same JSON
 * as RecyclerV2DumpState().  The caller must free() the returned
 * buffer.  Returns NULL on allocation failure.
 *
 * @return Heap-allocated JSON string (caller owns, free when done),
 *         or NULL on malloc failure.
 *
 * @code{.c}
 * char *json = RecyclerV2DumpStateToBuffer();
 * if (json) {
 *     printf("%s\n", json);
 *     free(json);
 * }
 * @endcode
 */
PUBLIC_API char *
RecyclerV2DumpStateToBuffer(void);

/**
 * @brief Describe recycler pool state as a JSON string (no json-c).
 *
 * Builds a JSON document describing every registered type pool — config,
 * capacity, per-group occupancy, and marshaller metadata.  When
 * @p type_pool_name is non-NULL, only the pool whose type name matches
 * exactly is emitted; otherwise every pool is described.
 *
 * The caller may supply a pre-initialised BufferDescriptor, or pass NULL to
 * have one allocated and initialised (256-byte initial capacity).  On
 * allocation failure NULL is returned.
 *
 * @param type_pool_name  Exact type name to filter on, or NULL for all pools.
 * @param provided        Caller-owned BufferDescriptor, or NULL to allocate.
 * @return The populated BufferDescriptor (the same pointer as @p provided
 *         when non-NULL), or NULL if a new descriptor could not be allocated.
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 512);
 * DescribeRecycler("Session", &bd);
 * printf("%s\n", bd.data);
 * BufferDescriptorRelease(&bd);
 * @endcode
 */
PUBLIC_API BufferDescriptor *
DescribeRecycler(const char *type_pool_name, BufferDescriptor *provided);

/**
 * @brief List every registered recycler type name in one contiguous allocation.
 *
 * Returns a freshly allocated CollectionDescriptor whose `collection` array
 * holds one pointer per registered type pool (in registration order), each
 * pointing at that pool's type name.  The descriptor, the pointer array, and
 * the name strings are all carved from a single malloc block, so the caller
 * releases everything with one `free()`.
 *
 * `collection_base_offset` is always 0 (elements are referenced by pointer,
 * not by value).  Each `collection[i]` is read back as `(const char *)`.
 *
 * @return Newly allocated CollectionDescriptor, or NULL on allocation failure.
 *         The caller owns it and must free() it exactly once when done.
 *
 * @code{.c}
 * CollectionDescriptor *types = ListRecyclerTypes();
 * if (types) {
 *     for (size_t i = 0; i < types->collection_sz; i++) {
 *         const char *name = (const char *)types->collection[i];
 *         // e.g. DescribeRecycler(name, &bd);
 *     }
 *     free(types);
 * }
 * @endcode
 */
PUBLIC_API CollectionDescriptor *
ListRecyclerTypes(void);

#endif /* UFLIB_RECYCLER_V2_RECYCLER_V2_H */
