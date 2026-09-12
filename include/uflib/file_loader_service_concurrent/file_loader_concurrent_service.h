/**
 * @file file_loader_concurrent_service.h
 * @brief FileLoaderService — public API for the concurrent lock-free
 *        file registry and loader.
 *
 * FileLoaderService is a bounded, lock-free registry that maps filesystem
 * paths to mmap'd file buffers.  A single internal monitor thread serialises
 * all I/O.  Consumer threads access loaded buffers via hazard-pointer-protected
 * zero-copy pointers.
 *
 * ## Thread safety
 *
 * Every operation is safe to call concurrently.  Destroy() must only be
 * called when no other threads are accessing the service.
 *
 * ## Integration pattern
 *
 * @code{.c}
 * // Embed in the server's master struct:
 * struct {
 *     FileLoaderServiceConcurrent file_loader;  // handle + config
 *     pthread_t                   thread;       // monitor thread
 *     bool                        is_enabled;   // consumer gate
 * } file_loader_service;
 *
 * // Init:
 * if (master.file_loader_service.is_enabled) {
 *     FileLoaderServiceInit(&master.file_loader_service.file_loader);
 *     master.file_loader_service.thread =
 *         FileLoaderServiceGetThread(
 *             master.file_loader_service.file_loader.service_handle);
 * }
 *
 * // Use:
 * void *data = FileLoaderServiceGetFileContent(
 *     svc.file_loader.service_handle, handle, &size);
 * FileLoaderServiceReturnFile(svc.file_loader.service_handle, data);
 *
 * // Shutdown:
 * FileLoaderServiceStop(svc.file_loader.service_handle);
 * pthread_join(svc.thread, NULL);
 * FileLoaderServiceDestroy(svc.file_loader.service_handle);
 * @endcode
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_FILE_LOADER_CONCURRENT_SERVICE_H
#define UFLIB_FILE_LOADER_CONCURRENT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include <uflib/uflib_defs.h>
#include <uflib/file_loader_service_concurrent/file_loader_service_concurrent_type.h>
#include <uflib/file_loader_service_concurrent/file_loader_service_file_info_type.h>

/* ── Handle type ──────────────────────────────────────────────────────── */

/*!
 * Opaque handle returned by AddFile().  Encodes pool index and generation
 * counter for stale-handle detection.
 */
typedef uint64_t FileLoaderServiceHandle;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

/**
 * @brief Initialise the file loader service.
 *
 * Reads config from @p config->service_config_params.  0 values trigger
 * compile-time defaults.  Allocates internal storage and spawns the
 * monitor thread.  On success, writes the opaque handle into
 * @p config->service_handle.
 *
 * @param config  Consumer-allocated config struct (zero-filled = defaults).
 * @return true on success.
 *
 * @code{.c}
 * FileLoaderServiceConcurrent fl = {0};
 * if (!FileLoaderServiceInit(&fl)) { ... }
 * // fl.service_handle is now set
 * @endcode
 */
PUBLIC_API bool
FileLoaderServiceInit(FileLoaderServiceConcurrent *config);

/**
 * @brief Tear down the service: stop monitor, join thread, free all memory.
 *
 * @param service_handle  Opaque handle from Init() (NULL-safe).
 */
PUBLIC_API void
FileLoaderServiceDestroy(FileLoaderService *service_handle);

/* ── File operations ──────────────────────────────────────────────────── */

PUBLIC_API FileLoaderServiceHandle
FileLoaderServiceAddFile(FileLoaderService *service_handle,
                          const char *path);

PUBLIC_API void
FileLoaderServiceRemoveFile(FileLoaderService *service_handle,
                             const char *path);

PUBLIC_API bool
FileLoaderServiceGetFileInfo(FileLoaderService *service_handle,
                              const char *path,
                              FileLoaderServiceFileInfo *out);

/* ── Buffer access ────────────────────────────────────────────────────── */

PUBLIC_API void *
FileLoaderServiceGetFileContent(FileLoaderService *service_handle,
                                 FileLoaderServiceHandle h,
                                 size_t *outSize);

PUBLIC_API void
FileLoaderServiceReturnFile(FileLoaderService *service_handle,
                             void *buf);

/* ── Control ──────────────────────────────────────────────────────────── */

PUBLIC_API void
FileLoaderServiceStop(FileLoaderService *service_handle);

/* ── Query ────────────────────────────────────────────────────────────── */

PUBLIC_API uint32_t
FileLoaderServiceSize(FileLoaderService *service_handle);

PUBLIC_API uint32_t
FileLoaderServiceCapacity(FileLoaderService *service_handle);

PUBLIC_API uint64_t
FileLoaderServiceHeartbeat(FileLoaderService *service_handle);

/* ── Thread ───────────────────────────────────────────────────────────── */

PUBLIC_API pthread_t
FileLoaderServiceGetThread(FileLoaderService *service_handle);

PUBLIC_API bool
FileLoaderServiceRegisterThread(FileLoaderService *service_handle);

PUBLIC_API void
FileLoaderServiceUnregisterThread(FileLoaderService *service_handle);

#endif /* UFLIB_FILE_LOADER_CONCURRENT_SERVICE_H */
