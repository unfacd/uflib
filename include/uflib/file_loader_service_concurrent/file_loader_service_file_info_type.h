/**
 * @file file_loader_service_file_info_type.h
 * @brief FileLoaderServiceFileInfo — public snapshot type.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_FILE_LOADER_SERVICE_FILE_INFO_TYPE_H
#define UFLIB_FILE_LOADER_SERVICE_FILE_INFO_TYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

/* ── File state ───────────────────────────────────────────────────────── */

typedef enum {
    FILE_LOADER_SERVICE_FILE_STATE_ACTIVE       = 0,
    FILE_LOADER_SERVICE_FILE_STATE_DELETED      = 1,
    FILE_LOADER_SERVICE_FILE_STATE_BLACKLISTED  = 2,
    FILE_LOADER_SERVICE_FILE_STATE_TOO_LARGE    = 3,
    FILE_LOADER_SERVICE_FILE_STATE_LOAD_ERROR   = 4,
    FILE_LOADER_SERVICE_FILE_STATE_EVICTED      = 5,
} FileLoaderServiceFileState;

/* ── File info snapshot ───────────────────────────────────────────────── */

/*!
 * Self-contained snapshot returned by FileLoaderServiceGetFileInfo().
 * All fields are populated by value — no hazard pointers or locks needed
 * after the call returns.
 */
typedef struct {
    bool                       in_registry;
    bool                       is_loaded;
    bool                       recently_accessed;
    FileLoaderServiceFileState state;
    size_t                     size_bytes;
    char                       path[PATH_MAX];
} FileLoaderServiceFileInfo;

#endif /* UFLIB_FILE_LOADER_SERVICE_FILE_INFO_TYPE_H */
