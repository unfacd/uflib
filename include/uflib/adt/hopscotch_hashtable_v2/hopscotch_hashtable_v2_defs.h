/**
 * @file
 * @brief Compile-time defaults for the HopscotchHashTable V2 module.
 *
 * Public (overridable) defaults use the CONFIG_DEFAULT_ prefix.
 * Private (internal) defaults use the PRIV_CONFIG_DEFAULT_ prefix.
 * Consumers can override CONFIG_DEFAULT_ values at compile time;
 * PRIV_CONFIG_DEFAULT_ values are not part of the public API contract.
 */

#ifndef UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_DEFS_H
#define UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_DEFS_H

/*! Default neighbourhood size in slots (hopinfo bitmap width). */
#define CONFIG_DEFAULT_HOPSCOTCH_HOP_RANGE             32

/*! Default log2(initial bucket count).  6 -> 64 buckets. */
#define CONFIG_DEFAULT_HOPSCOTCH_INIT_PFACTOR          6

/*! Default key width in bytes.  sizeof(uint64_t) = 8 on LP64. */
#define CONFIG_DEFAULT_HOPSCOTCH_KEY_WIDTH             sizeof(uint64_t)

/* ── Private defaults — not part of the public API contract ────────────── */

/*! Maximum key width supported (bytes). */
#define PRIV_CONFIG_DEFAULT_HOPSCOTCH_MAX_KEY_WIDTH    64

/*! Resize multiplier — each resize doubles the table. */
#define PRIV_CONFIG_DEFAULT_HOPSCOTCH_RESIZE_DELTA     1

#endif /* UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_DEFS_H */
