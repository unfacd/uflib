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
 * @file recycler_v2_type.h
 * @brief Public type definitions for RecyclerV2 — consumer-visible types only
 *
 * This header contains ONLY type definitions — no function declarations.
 * Consumers that only need to forward-declare or embed a RecyclerV2 type
 * should include this file rather than the full recycler_v2.h.
 */

#ifndef UFLIB_RECYCLER_V2_RECYCLER_V2_TYPE_H
#define UFLIB_RECYCLER_V2_RECYCLER_V2_TYPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <uflib/main_types.h>
#include <uflib/uflib_defs.h>
#include <uflib/recycler_v2/instance_holder_v2_type.h>
#include <uflib/logger/logger_type.h>

/* ── Opaque context type ──────────────────────────────────────────────────── */

/** Opaque context — consumer-defined data passed through to callbacks. */
typedef void ContextData;

/* ── Forward declarations (opaque — implementation hidden in _priv.h) ────── */

/*! Opaque type — singleton recycler root. Consumer never sees the definition. */
typedef struct RecyclerV2 RecyclerV2;

/*! Opaque type — per-type pool definition. Consumer never sees the definition. */
typedef struct RecyclerV2PoolDefinition RecyclerV2PoolDefinition;

/*! Opaque type — one contiguous memory chunk + its Treiber free stack. */
typedef struct AllocationGroupV2 AllocationGroupV2;

/*! Opaque type — management header prefixed to every pool-allocated object. */
typedef struct RecyclerV2PoolTypeEnvelop RecyclerV2PoolTypeEnvelop;

/* ── Public handle types ──────────────────────────────────────────────────── */

/*! Opaque handle returned by RecyclerV2InitTypePool().
 *  The consumer reads ->type for the numeric type identifier (1..65535). */
typedef struct RecyclerV2PoolHandle {
    uint16_t    type;         ///< 1-based type index, globally unique
    const char *type_name;    ///< human-readable type name (e.g. "Session")
    size_t      blocksz;      ///< sizeof(consumer object)
} RecyclerV2PoolHandle;

/* ── Callback typedefs ────────────────────────────────────────────────────── */

/**
 * @brief Called once per object during group expansion.
 * @param data_ptr  Pointer to the newly allocated object payload.
 * @param oid       Unique object ID, fixed for the object's lifetime.
 * @return 0 on success, non-zero on failure.
 */
typedef int (*RecyclerV2PoolOpInitCallback)(ClientContextData *data_ptr, size_t oid);

/**
 * @brief Called on every RecyclerV2Get() after dequeuing an object.
 * @param ih_ptr      InstanceHolderV2 wrapping the object.
 * @param context_ptr  Opaque context passed by the consumer.
 * @param oid          Unique object ID.
 * @param call_flags   Type-specific flags.
 * @return 0 on success, non-zero to reject the object (it is returned to pool).
 */
typedef int (*RecyclerV2PoolOpInitGetCallback)(InstanceHolderV2 *ih_ptr,
    ContextData *context_ptr, size_t oid, unsigned long call_flags);

/**
 * @brief Called on every RecyclerV2Put() before enqueuing the object.
 * @param ih_ptr      InstanceHolderV2 wrapping the object.
 * @param context_ptr  Opaque context passed by the consumer.
 * @param call_flags   Type-specific flags.
 * @return 0 on success, non-zero on failure.
 */
typedef int (*RecyclerV2PoolOpInitPutCallback)(InstanceHolderV2 *ih_ptr,
    ContextData *context_ptr, unsigned long call_flags);

/**
 * @brief Called during object destruction.
 * @param ih_ptr      InstanceHolderV2 wrapping the object.
 * @param context_ptr  Opaque context.
 * @param call_flags   Type-specific flags.
 * @return 0 on success.
 */
typedef int (*RecyclerV2PoolOpDestructCallback)(InstanceHolderV2 *ih_ptr,
    ContextData *context_ptr, unsigned long call_flags);

/* ── Marshaller callbacks (V2 only) ───────────────────────────────────────── */

/**
 * @brief Serialize an object payload into a recycler-provided buffer.
 *
 * The recycler owns the filesystem I/O: it allocates a buffer of
 * marshal_blob_max_sz bytes (declared by the consumer at pool init),
 * calls this callback to fill it, then writes the bytes to persistent
 * storage atomically.  The consumer never touches a file descriptor,
 * never knows the storage layout, and never allocates or frees the buffer.
 *
 * @param obj_ptr     Pointer to the object payload to serialize.
 * @param out_buf     Recycler-allocated buffer (marshal_blob_max_sz bytes).
 * @param buf_sz      Size of out_buf in bytes (consumer-declared at init).
 * @param out_len_ptr Receives the number of bytes actually written.
 * @return 0 on success, non-zero on failure.
 */
typedef int (*RecyclerV2PoolOpMarshalCallback)(ClientContextData *obj_ptr,
    uint8_t *out_buf, size_t buf_sz, size_t *out_len_ptr);

/**
 * @brief Re-materialize an object from a serialized blob.
 *
 * The recycler reads the blob from persistent storage, pops a fresh pool
 * object, and calls this callback to populate it.  The consumer deserializes
 * from the provided buffer — it never reads from disk, never knows the
 * storage layout, and never allocates or frees the buffer.
 *
 * @param obj_ptr    Pre-allocated object payload to populate.
 * @param in_buf     Serialized bytes read from recycler-managed storage.
 * @param buf_len    Number of valid bytes in in_buf.
 * @return 0 on success, non-zero on failure.
 */
typedef int (*RecyclerV2PoolOpUnmarshalCallback)(ClientContextData *obj_ptr,
    const uint8_t *in_buf, size_t buf_len);

/**
 * @brief Optional: provide a wall-clock timestamp of the object's last
 *        genuine use for LRU tiebreaking during CLOCK eviction.
 *
 * @param ih_ptr  InstanceHolderV2 wrapping the object.
 * @return Timestamp (monotonic, in whatever units the consumer chooses).
 *         Return 0 if not implemented — the recycler falls back to its
 *         own coarse timestamp.
 */
typedef uint64_t (*RecyclerV2PoolOpLastUsedCallback)(InstanceHolderV2 *ih_ptr);

/**
 * @brief Optional: clean up marshalled state after successful re-materialization.
 *
 * Called after the unmarshal callback succeeds and the InstanceHolderV2 has
 * been transitioned back to Instance mode.  The marshalled blob on disk has
 * already been deleted by the recycler — this callback is for consumer-side
 * cleanup (e.g., freeing auxiliary structures, closing file descriptors).
 *
 * @param marshaller_id  The now-resolved marshaller ID.
 */
typedef void (*RecyclerV2PoolOpMarshalCleanupCallback)(MarshallerContextData marshaller_id);

/* ── Operations struct ────────────────────────────────────────────────────── */

/*! Lifecycle and marshaller callbacks registered per type pool.
 *  All callbacks are optional — set to NULL if not needed. */
typedef struct RecyclerV2PoolOps {
    /* Core lifecycle (same pattern as V1) */
    RecyclerV2PoolOpInitCallback              poolop_init_callback;
    RecyclerV2PoolOpInitGetCallback           poolop_initget_callback;
    RecyclerV2PoolOpInitPutCallback           poolop_initput_callback;
    RecyclerV2PoolOpDestructCallback          poolop_destruct_callback;

    /* Marshaller callbacks (new in V2 — NULL if marshal not supported) */
    RecyclerV2PoolOpMarshalCallback           poolop_marshal_callback;
    RecyclerV2PoolOpUnmarshalCallback         poolop_unmarshal_callback;
    RecyclerV2PoolOpLastUsedCallback          poolop_last_used_callback;
    RecyclerV2PoolOpMarshalCleanupCallback    poolop_marshal_cleanup_callback;
} RecyclerV2PoolOps;

/* ── Configuration struct ─────────────────────────────────────────────────── */

/** Policy for handling stale blobs on storage init (server restart). */
typedef enum RecyclerV2StorageInitPolicy {
    RECYCLER_V2_STORAGE_OVERWRITE = 0,  ///< Delete old blobs, start clean
    RECYCLER_V2_STORAGE_APPEND    = 1,  ///< Keep old blobs (orphaned — IDs are monotonic)
    RECYCLER_V2_STORAGE_ARCHIVE   = 2,  ///< Move old blobs to <root>/archived/<type>_<timestamp>/
} RecyclerV2StorageInitPolicy;

/*! Configuration struct passed to RecyclerV2InitTypePool().
 *  Zero/NULL fields trigger compile-time defaults from recycler_v2_defs.h. */
typedef struct RecyclerV2PoolConfig {
    const char                  *type_name;            ///< Human-readable type name (e.g. "Session")
    size_t                       blocksz;              ///< sizeof(consumer object payload)
    uint32_t                     group_allocation_sz;  ///< Max number of allocation groups (0 → default)
    uint32_t                     expansion_threshold;  ///< Objects per group (0 → default)
    RecyclerV2PoolOps           *ops_ptr;              ///< Lifecycle + marshaller callbacks (NULL → all-default)
    float                        marshal_watermark;    ///< 0.0–1.0 trigger threshold (0.0 → default 1.0)
    size_t                       marshal_blob_max_sz;  ///< Max serialized object size in bytes (0 → marshalling disabled)
    const char                  *storage_root;         ///< Root dir for marshalled blobs (NULL → default /opt/ufsrv/var/recycler/blobs)
    const char                  *ufsrv_class;          ///< Server class e.g. "ufsrvwebsock" (NULL → no class subdirectory)
    const char                  *instance_id;          ///< Instance discriminator e.g. "1", "east" (NULL → no instance subdirectory)
    RecyclerV2StorageInitPolicy  storage_init_policy;  ///< Stale-blob policy on init (OVERWRITE / APPEND / ARCHIVE)
    UfLogger                    *logger_ptr;           ///< Borrowed diagnostic sink (NULL → no reporting); see DescribeRecycler()
} RecyclerV2PoolConfig;

#endif /* UFLIB_RECYCLER_V2_RECYCLER_V2_TYPE_H */
