/**
 * @file file_loader_service_concurrent_type.h
 * @brief FileLoaderServiceConcurrent — public type for consumer embedding.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_FILE_LOADER_SERVICE_CONCURRENT_TYPE_H
#define UFLIB_FILE_LOADER_SERVICE_CONCURRENT_TYPE_H

#include <stdint.h>

/* ── Opaque handle ────────────────────────────────────────────────────── */

typedef void FileLoaderService;

/*!
 * Consumer-facing config + handle struct.
 *
 * Embed this in the server's master struct.  Populate service_config_params
 * from Lua config (or pass all zeros for compile-time defaults), then call
 * FileLoaderServiceInit().  On success, service_handle is set.
 */
typedef struct {
    FileLoaderService *service_handle;       ///< opaque, set by Init()

    struct {
        uint32_t     registry_size;          ///< max files tracked (0 = default)
        uint32_t     loaded_size;            ///< max files mmap'd (0 = default)
        const char **blacklist_entries;      ///< paths never served (or NULL)
        uint32_t     blacklist_entry_count;  ///< number of entries
    } service_config_params;
} FileLoaderServiceConcurrent;

#endif /* UFLIB_FILE_LOADER_SERVICE_CONCURRENT_TYPE_H */
