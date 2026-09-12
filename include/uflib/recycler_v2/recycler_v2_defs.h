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
 * @file recycler_v2_defs.h
 * @brief Compile-time configuration defaults for RecyclerV2 — lock-free slab allocator
 *
 * Naming convention per UFSRV_CODING_CONVENTIONS.md §Compile-time defaults:
 *   CONFIG_DEFAULT_*    — may be overridable at configure time
 *   PRIV_CONFIG_DEFAULT_* — never overridable, private implementation limits
 */

#ifndef UFLIB_RECYCLER_V2_RECYCLER_V2_DEFS_H
#define UFLIB_RECYCLER_V2_RECYCLER_V2_DEFS_H
#include <stdint.h>

/* ── Type registry configuration ─────────────────────────────────────────── */

/** Number of type slots to allocate when the recycler's type registry grows. */
#define CONFIG_DEFAULT_RECYCLER_V2_TYPE_SLOT_EXPANSION  10

/** Default soft watermark for marshal trigger (1.0 = hard exhaustion only). */
#define CONFIG_DEFAULT_RECYCLER_V2_MARSHAL_WATERMARK    1.0f

/* ── Expansion configuration ─────────────────────────────────────────────── */

/** Default number of allocation groups per type pool (0 = unlimited up to memory). */
#define CONFIG_DEFAULT_RECYCLER_V2_MAX_ALLOCATION_GROUPS 16

/** Default number of objects per allocation group. */
#define CONFIG_DEFAULT_RECYCLER_V2_EXPANSION_THRESHOLD   1024

/* ── InstanceHolderV2 configuration ──────────────────────────────────────── */

/** Byte alignment requirement for InstanceHolderV2 pointers. */
#define PRIV_CONFIG_DEFAULT_RECYCLER_V2_INSTANCE_BYTE_ALIGNMENT  8

/** Default tag value applied to InstanceHolderV2 instances (2 bits, values 0–1). */
#define PRIV_CONFIG_DEFAULT_RECYCLER_V2_INSTANCE_TAG             1

/** Maximum tag value (2 bits). */
#define PRIV_CONFIG_DEFAULT_RECYCLER_V2_INSTANCE_MAX_TAG         1

/* ── Marshaller configuration ─────────────────────────────────────────────── */

/** Filesystem root for recycler-managed marshalled object storage.
 *  Overridable per-pool via RecyclerV2PoolConfig::storage_root.
 *  In production this is typically /opt/ufsrv/var/recycler/blobs. */
#define CONFIG_DEFAULT_RECYCLER_V2_STORAGE_ROOT         "/opt/ufsrv/var/recycler/blobs"

/** Default stale-blob policy on storage init.
 *  OVERWRITE is safe — marshaller IDs restart from 1, so old blobs
 *  from a prior process lifetime can never be resolved. */
#define CONFIG_DEFAULT_RECYCLER_V2_STORAGE_INIT_POLICY   RECYCLER_V2_STORAGE_OVERWRITE

/** Maximum size in bytes of a serialized object blob.
 *  The recycler allocates a buffer of this size on the stack during marshal
 *  and on the heap during unmarshal.  Must be large enough to hold the
 *  protobuf-serialized representation of the largest consumer object. */
#define CONFIG_DEFAULT_RECYCLER_V2_MARSHAL_BLOB_MAX_SZ  16384

/** Number of bits allocated to marshaller_id in the tagged pointer. */
#define PRIV_CONFIG_DEFAULT_RECYCLER_V2_MARSHALLER_ID_BITS       47
#define RECYCLER_V2_MARSHALLER_ID_MAX ((UINT64_C(1) << PRIV_CONFIG_DEFAULT_RECYCLER_V2_MARSHALLER_ID_BITS) - 1)

/** Number of bits allocated to type_index in the marshaller encoding. */
#define PRIV_CONFIG_DEFAULT_RECYCLER_V2_MARSHALLER_TYPE_BITS     16

/* ── CLOCK sweep configuration ───────────────────────────────────────────── */

/** Maximum objects to scan in a single CLOCK sweep pass. */
#define PRIV_CONFIG_DEFAULT_RECYCLER_V2_CLOCK_MAX_SWEEP          4096

/* ── Treiber stack configuration ─────────────────────────────────────────── */

/** Number of bits for pop_count in tagged-pointer free stack head. */
#define PRIV_CONFIG_DEFAULT_RECYCLER_V2_FREE_STACK_POPCOUNT_BITS 32

/* ── Refcount sentinel values ────────────────────────────────────────────── */

#define RECYCLER_V2_REFCOUNT_AVAILABLE  1   ///< Envelope is on free stack, available for allocation
#define RECYCLER_V2_REFCOUNT_LEASED     2   ///< Envelope is leased to consumer (base + 1 active ref)

#endif /* UFLIB_RECYCLER_V2_RECYCLER_V2_DEFS_H */
