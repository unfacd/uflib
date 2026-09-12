/**
 * @file file_loader_concurrent_access_defs.h
 * @brief FileLoaderConcurrentAccess — compile-time configuration defaults.
 *
 * CONFIG_DEFAULT_* constants may be overridable at CMake configure time
 * via a generated config.h.  PRIV_CONFIG_DEFAULT_* constants are not.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_THREAD_WORKER_FILE_LOADER_CONCURRENT_ACCESS_DEFS_H
#define UFLIB_THREAD_WORKER_FILE_LOADER_CONCURRENT_ACCESS_DEFS_H

#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_POOL_CAPACITY          4096U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BUFFER_CACHE_SZ        64U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MAX_FILE_SIZE_BYTES    (1024U * 1024U)
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HASHMAP_LOAD_FACTOR    75U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_BLACKLIST_CAPACITY     1024U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_MONITOR_TIMEOUT_MS     500U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_CONSUMER_TIMEOUT_MS    5000U
#define CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_HAZPTR_SLOTS           128U
#define PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_LIMBO_SZ          512U
#define PRIV_CONFIG_DEFAULT_FILELOADERCONCURRENTACCESS_KEY_WIDTH         256U

#endif /* UFLIB_THREAD_WORKER_FILE_LOADER_CONCURRENT_ACCESS_DEFS_H */
