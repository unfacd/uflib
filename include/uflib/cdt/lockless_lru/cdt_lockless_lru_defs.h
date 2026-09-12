/**
 * @file cdt_lockless_lru_defs.h
 * @brief Module-specific preprocessor definitions for the lockless LRU cache.
 *
 * These constants are consumed by cdt_lockless_lru.h and cdt_lockless_lru.c.
 * Override the CONFIG_DEFAULT_* values via a generated config_uflib.h if needed.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_DEFS_H
#define UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_DEFS_H

/*!
 * Default target capacity (max items the cache should hold).
 *
 * This is a soft bound — under concurrent insertion the cache may
 * temporarily hold more than this many entries.  The physical slot
 * capacity provides the true hard ceiling.
 */
#ifndef CONFIG_DEFAULT_LOCKLESS_LRU_CAPACITY_HINT
  #define CONFIG_DEFAULT_LOCKLESS_LRU_CAPACITY_HINT  256
#endif

/*!
 * Multiplier applied to capacity_hint to compute the physical slot count.
 *
 * A multiplier of 2 means the physical table is twice the logical capacity,
 * leaving headroom for linear probing to stay efficient.
 */
#ifndef CONFIG_DEFAULT_LOCKLESS_LRU_PROBE_HEADROOM_MULT
  #define CONFIG_DEFAULT_LOCKLESS_LRU_PROBE_HEADROOM_MULT  2
#endif

/*!
 * Minimum physical slot count, enforced regardless of capacity_hint.
 *
 * Prevents pathological behaviour with tiny capacity_hint values.
 */
#ifndef PRIV_CONFIG_DEFAULT_LOCKLESS_LRU_MIN_SLOTS
  #define PRIV_CONFIG_DEFAULT_LOCKLESS_LRU_MIN_SLOTS  8
#endif

/*!
 * Maximum CLOCK sweep length, as a multiple of the physical capacity.
 *
 * One full sweep clears every `referenced` bit (second chance); a second
 * sweep evicts whatever is left.  Bounding the sweep guarantees Set()
 * cannot livelock on a hot working set that keeps re-setting `referenced`.
 */
#ifndef PRIV_CONFIG_LOCKLESS_LRU_CLOCK_SWEEP_MULT
  #define PRIV_CONFIG_LOCKLESS_LRU_CLOCK_SWEEP_MULT  2
#endif

#endif /* UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_DEFS_H */
