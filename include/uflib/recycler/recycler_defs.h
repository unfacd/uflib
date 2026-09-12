/**
 * @file recycler_defs.h
 * @brief Compile-time configuration defaults for the Recycler slab allocator
 *
 * Naming convention per UFSRV_CODING_CONVENTIONS.md §Compile-time defaults:
 *   CONFIG_DEFAULT_*    — may be overridable at configure time
 *   PRIV_CONFIG_DEFAULT_* — never overridable, private implementation limits
 */

#ifndef UFLIB_RECYCLER_RECYCLER_DEFS_H
#define UFLIB_RECYCLER_RECYCLER_DEFS_H

/* ── Expansion configuration ──────────────────────────────────────────── */

/** Number of type slots to allocate when the recycler's type registry grows. */
#define CONFIG_DEFAULT_RECYCLER_TYPE_SLOT_EXPANSION  10

/** Default soft watermark for marshal trigger (1.0 = hard exhaustion only). */
#define CONFIG_DEFAULT_RECYCLER_MARSHAL_WATERMARK    1.0f

/* ── InstanceHolder configuration ─────────────────────────────────────── */

/** Byte alignment requirement for InstanceHolder pointers. */
#define PRIV_CONFIG_DEFAULT_INSTANCE_BYTE_ALIGNMENT  8

/** Default tag value applied to InstanceHolder instances. */
#define PRIV_CONFIG_DEFAULT_INSTANCE_TAG             1

/** Maximum tag value (2 bits for InstanceHolder). */
#define PRIV_CONFIG_DEFAULT_INSTANCE_MAX_TAG         1

/* ── Marshaller configuration ─────────────────────────────────────────── */

/** Filesystem root for recycler-managed marshalled object storage. */
#define CONFIG_DEFAULT_RECYCLER_STORAGE_ROOT         "/var/run/ufsrv/recycler"

/** Number of bits allocated to marshaller_id in the tagged pointer. */
#define PRIV_CONFIG_DEFAULT_MARSHALLER_ID_BITS       47

#endif /* UFLIB_RECYCLER_RECYCLER_DEFS_H */
