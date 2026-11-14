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
 *     bool                        is_enabled;   // consumer gate
 * } file_loader_service;
 *
 * // Init:
 * if (master.file_loader_service.is_enabled) {
 *     FileLoaderServiceInit(&master.file_loader_service.file_loader);
 * }
 *
 * // Use:
 * void *data = FileLoaderServiceGetFileContent(
 *     svc.file_loader.service_handle, handle, &size);
 * FileLoaderServiceReturnFile(svc.file_loader.service_handle, data);
 *
 * // Shutdown:
 * FileLoaderServiceStop(svc.file_loader.service_handle);
 * FileLoaderServiceDestroy(svc.file_loader.service_handle);
 * @endcode
 *
 * Stop() signals the monitor and reaps it, so the caller never joins the
 * monitor thread itself; Destroy() joins only if Stop() was not called.
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
#include <uflib/logger/logger_type.h>
#include <uflib/file_loader_service_concurrent/file_loader_service_concurrent_type.h>
#include <uflib/file_loader_service_concurrent/file_loader_service_file_info_type.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>

/* ── Handle type ──────────────────────────────────────────────────────── */

/*!
 * Opaque handle returned by AddFile().  Encodes pool index and generation
 * counter for stale-handle detection.
 */
typedef uint64_t FileLoaderServiceFileHandle;

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
 * @brief Initialise the service with a consumer-supplied logger.
 *
 * @param config      Consumer-allocated config struct.
 * @param logger_ptr  Logger inherited by the monitor thread (NULL allowed).
 * @return true on success; @p config->service_handle is then set.
 */
PUBLIC_API bool
FileLoaderServiceInitWithLogger(FileLoaderServiceConcurrent *config, UfLogger *logger_ptr);

/**
 * @brief Tear down the service: stop monitor, join thread, free all memory.
 *
 * @param service_handle  Opaque handle from Init() (NULL-safe).
 */
PUBLIC_API void
FileLoaderServiceDestroy(FileLoaderService *service_handle);

/* ── File operations ──────────────────────────────────────────────────── */

/**
 * @brief Register @p path and return its file handle.
 *
 * Idempotent: an already-registered path returns its existing handle.  The
 * monitor loads the file asynchronously; GetFileContent() waits for it to be
 * published.  Blacklisted paths are refused.
 *
 * @param service_handle  Opaque handle from Init().
 * @param path            Filesystem path (copied into the registry).
 * @return File handle, or 0 when blacklisted or the registry rejects it.
 */
PUBLIC_API FileLoaderServiceFileHandle
FileLoaderServiceAddFile(FileLoaderService *service_handle,
                          const char *path);

/**
 * @brief Remove @p path from the registry and retire its mapping.
 *
 * Handles obtained for the entry become stale (ESTALE).  NULL-safe.
 *
 * @param service_handle  Opaque handle from Init().
 * @param path            Registered path to drop.
 */
PUBLIC_API void
FileLoaderServiceRemoveFile(FileLoaderService *service_handle,
                             const char *path);

/**
 * @brief Report registry state for @p path.
 *
 * @param service_handle  Opaque handle from Init().
 * @param path            Path to inspect.
 * @param out             Receives path, registry/load flags and state; it is
 *                        zeroed first.
 * @return true unless the arguments are invalid.  A blacklisted path returns
 *         true with @c state BLACKLISTED and @c in_registry false.
 */
PUBLIC_API bool
FileLoaderServiceGetFileInfo(FileLoaderService *service_handle,
                              const char *path,
                              FileLoaderServiceFileInfo *out);

/* ── Buffer access ────────────────────────────────────────────────────── */

/**
 * @brief Return the published buffer for @p h.
 *
 * Waits for the monitor to publish the file, up to the configured consumer
 * timeout.  The buffer is pinned in the caller's hazard slot and must be
 * released with FileLoaderServiceReturnFile() on the same thread.
 *
 * @param service_handle  Opaque handle from Init().
 * @param h               Handle from AddFile().
 * @param outSize         Receives the byte length; set to 0 on failure.
 * @return Buffer, or NULL with errno set (EINVAL, ENOENT, ESTALE, ETIMEDOUT).
 */
PUBLIC_API void *
FileLoaderServiceGetFileContent(FileLoaderService *service_handle,
                                 FileLoaderServiceFileHandle h,
                                 size_t *outSize);

/**
 * @brief Fetch a registered file's content by path.
 *
 * Resolves @p path against the registry and returns the published buffer.
 * Blacklisted paths are refused before resolution.  This is the path-based
 * counterpart to FileLoaderServiceGetFileContent(), which requires a
 * FileLoaderServiceFileHandle.
 *
 * @param service_handle  Opaque handle from Init().
 * @param path            Registry path as supplied to AddFile().
 * @param outSize         Receives the buffer size; set to 0 on failure.
 * @return The buffer (release with FileLoaderServiceReturnFile()), or NULL
 *         with errno set: EACCES when blacklisted, ENOENT when the path is
 *         not registered, EINVAL for invalid arguments, and ESTALE/ETIMEDOUT
 *         as documented for FileLoaderServiceGetFileContent().
 */
PUBLIC_API void *
FileLoaderServiceGetFileContentByPath(FileLoaderService *service_handle,
                                      const char *path,
                                      size_t *outSize);

/**
 * @brief Release a buffer obtained from GetFileContent[ByPath]().
 *
 * Clears the caller's hazard slot; the mapping then becomes reclaimable.
 * Call it on the acquiring thread, exactly once, with the returned pointer.
 * NULL-safe.
 *
 * @param service_handle  Opaque handle from Init().
 * @param buf             Pointer returned by a content getter.
 */
PUBLIC_API void
FileLoaderServiceReturnFile(FileLoaderService *service_handle, void *buf);

/* ── Control ──────────────────────────────────────────────────────────── */

/**
 * @brief Signal and reap the monitor thread.
 *
 * Idempotent and NULL-safe; Destroy() joins only when this was not called.
 *
 * @param service_handle  Opaque handle from Init().
 */
PUBLIC_API void
FileLoaderServiceStop(FileLoaderService *service_handle);

/* ── Query ────────────────────────────────────────────────────────────── */

/** @brief Number of paths currently registered. */
PUBLIC_API uint32_t
FileLoaderServiceSize(FileLoaderService *service_handle);

/** @brief Maximum number of registered paths (registry capacity). */
PUBLIC_API uint32_t
FileLoaderServiceCapacity(FileLoaderService *service_handle);

/** @brief Monitor iteration counter; 0 before the monitor starts. */
PUBLIC_API uint64_t
FileLoaderServiceHeartbeat(FileLoaderService *service_handle);

/**
 * @brief Render the service configuration and state as JSON.
 *
 * @param service_handle  Opaque handle from Init() (NULL-safe).
 * @param provided        Caller buffer, or NULL for a freshly allocated one.
 * @return The descriptor holding the JSON, or NULL without a service.
 */
PUBLIC_API BufferDescriptor *
FileLoaderServiceDescribe(const FileLoaderService *service_handle, BufferDescriptor *provided);

/* ── Thread ───────────────────────────────────────────────────────────── */

/** @brief Monitor thread id, or 0 once Stop()/Destroy() has reaped it. */
PUBLIC_API pthread_t
FileLoaderServiceGetThread(FileLoaderService *service_handle);

/**
 * @brief Reserve the calling thread's hazard slot.
 *
 * Optional: content getters register lazily on first use.  A thread holds one
 * slot, so concurrent fetches on the same thread share it.
 *
 * @param service_handle  Opaque handle from Init().
 * @return true when a slot is held after the call.
 */
PUBLIC_API bool
FileLoaderServiceRegisterThread(FileLoaderService *service_handle);

/**
 * @brief Release the calling thread's hazard slot.
 *
 * @param service_handle  Opaque handle from Init() (NULL-safe).
 */
PUBLIC_API void
FileLoaderServiceUnregisterThread(FileLoaderService *service_handle);

#endif /* UFLIB_FILE_LOADER_CONCURRENT_SERVICE_H */
