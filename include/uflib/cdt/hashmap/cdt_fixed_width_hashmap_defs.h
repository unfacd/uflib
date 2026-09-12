/**
 * @file cdt_hashmap_defs.h
 * @brief Module-specific preprocessor definitions for the lockless hash map.
 *
 * These constants are consumed by cdt_hashmap.h and cdt_hashmap.c.
 * Override the CONFIG_DEFAULT_* values via a generated config_uflib.h if needed.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_HASHMAP_CDT_FIXED_WIDTH_HASHMAP_DEFS_H
#define UFLIB_CDT_HASHMAP_CDT_FIXED_WIDTH_HASHMAP_DEFS_H

/*!
 * Default key width in bytes.
 *
 * Every pool node carries this many bytes for the key.  Shorter keys are
 * NUL-padded; longer keys are silently truncated.  Consumers that need a
 * different width can override at compile time.
 */
#ifndef CONFIG_DEFAULT_CDS_HASHMAP_KEY_WIDTH
  #define CONFIG_DEFAULT_CDS_HASHMAP_KEY_WIDTH  256
#endif

/*!
 * Default pool sizing factor as a percentage of slot capacity.
 *
 * A load factor of 75 % means the pool holds 75 % as many nodes as there are
 * hash slots, leaving headroom for linear probing to stay efficient.
 */
#ifndef CONFIG_DEFAULT_CDS_HASHMAP_LOAD_FACTOR_PCT
  #define CONFIG_DEFAULT_CDS_HASHMAP_LOAD_FACTOR_PCT  75
#endif

/*!
 * Maximum linear-probe distance before the map is considered pathologically
 * full (cluster).  Insertions fail when this limit is reached.
 */
#ifndef PRIV_CONFIG_DEFAULT_CDS_HASHMAP_MAX_PROBE
  #define PRIV_CONFIG_DEFAULT_CDS_HASHMAP_MAX_PROBE  512
#endif

/*!
 * Number of retire-list entries to accumulate before scanning for nodes
 * that are safe to recycle back to the free stack.
 */
#ifndef PRIV_CONFIG_DEFAULT_CDS_HASHMAP_RETIRE_BATCH
  #define PRIV_CONFIG_DEFAULT_CDS_HASHMAP_RETIRE_BATCH  64
#endif

#endif /* UFLIB_CDT_HASHMAP_CDT_FIXED_WIDTH_HASHMAP_DEFS_H */
