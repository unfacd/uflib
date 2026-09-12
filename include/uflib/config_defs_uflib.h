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
 * @file config_defs_uflib.h
 * @brief Compile-time configuration defaults for uflib.
 *
 * CONFIG_DEFAULT_* constants may be overridable at CMake configure time
 * via a generated config_uflib.h.  PRIV_CONFIG_DEFAULT_* constants are not.
 */

#ifndef UFLIB_CONFIG_DEFS_UFLIB_H
#define UFLIB_CONFIG_DEFS_UFLIB_H

#define CONFIG_DEFAULT_EMAIL_ADDRESS_SZ_MAX               320
#define CONFIG_DEFAULT_EMAIL_ADDRESS_CONSERVATIVE_SZ_MAX  64  // practical limit https://atdata.com/blog/long-email-addresses/
#define CONFIG_DEFAULT_EMAIL_ADDRESS_SZ_MIN               6   // a@b.co

// Legacy aliases — code still references the unprefixed names.
// TODO: migrate call sites to CONFIG_DEFAULT_* names during Phase A12.
#define CONFIG_EMAIL_ADDRESS_SZ_MAX               CONFIG_DEFAULT_EMAIL_ADDRESS_SZ_MAX
#define CONFIG_EMAIL_ADDRESS_CONSERVATIVE_SZ_MAX  CONFIG_DEFAULT_EMAIL_ADDRESS_CONSERVATIVE_SZ_MAX
#define CONFIG_EMAIL_ADDRESS_SZ_MIN               CONFIG_DEFAULT_EMAIL_ADDRESS_SZ_MIN

/* ── Lockless hash map defaults ────────────────────────────────────────── */

#define CONFIG_DEFAULT_CDS_HASHMAP_KEY_WIDTH         256
#define CONFIG_DEFAULT_CDS_HASHMAP_LOAD_FACTOR_PCT   75
#define PRIV_CONFIG_DEFAULT_CDS_HASHMAP_MAX_PROBE    512
#define PRIV_CONFIG_DEFAULT_CDS_HASHMAP_RETIRE_BATCH 64

/* ── FileLoaderConcurrentAccess defaults ──────────────────────────────────── */

#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_POOL_CAPACITY          4096U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BUFFER_CACHE_SZ        64U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MAX_FILE_SIZE_BYTES    (1024U * 1024U)
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HASHMAP_LOAD_FACTOR    75U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BLACKLIST_CAPACITY     1024U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MONITOR_TIMEOUT_MS     500U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_CONSUMER_TIMEOUT_MS    5000U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HAZPTR_SLOTS           128U
#define PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_LIMBO_SZ          512U

#endif /* UFLIB_CONFIG_DEFS_UFLIB_H */
