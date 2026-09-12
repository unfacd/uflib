/**
 * @file file_loader_concurrent_access_type.h
 * @brief FileLoaderConcurrentAccess — type definitions for the concurrent
 *        lock-free file registry and loader.
 *
 * This header contains the struct definitions, enums, typedefs, and embedded
 * constants.  Consumers that only need the type (e.g. for embedding in a
 * master server struct) can include just this file without pulling in the
 * full API surface.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_FILE_LOADER_SERVICE_TYPE_PRIV_H
#define UFLIB_FILE_LOADER_SERVICE_TYPE_PRIV_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>

#include <uflib/cdt/hashmap/lockless_fixed_width_hashmap_type.h>
#include <uflib/cdt/cdt_mpsc_queue_type.h>

/* ── Handle encoding ─────────────────────────────────────────────────────── */

#define FILELOADERCONCURRENTACCESS_HANDLE_IDX_SHIFT  44U
#define FILELOADERCONCURRENTACCESS_HANDLE_GEN_MASK \
    ((1ULL << FILELOADERCONCURRENTACCESS_HANDLE_IDX_SHIFT) - 1ULL)

/* ── File state enum ──────────────────────────────────────────────────────── */

/*!
 * Lifecycle state of a tracked file.
 */
typedef enum {
    FILELOADERCONCURRENTACCESS_FILE_STATE_ACTIVE       = 0,
    FILELOADERCONCURRENTACCESS_FILE_STATE_DELETED      = 1,
    FILELOADERCONCURRENTACCESS_FILE_STATE_BLACKLISTED  = 2,
    FILELOADERCONCURRENTACCESS_FILE_STATE_TOO_LARGE    = 3,
    FILELOADERCONCURRENTACCESS_FILE_STATE_LOAD_ERROR   = 4,
    FILELOADERCONCURRENTACCESS_FILE_STATE_EVICTED      = 5,
} FileLoaderConcurrentAccessFileState;

/* ── Result enum ──────────────────────────────────────────────────────────── */

/*!
 * Return codes for operations that can fail.
 */
typedef enum {
    FILELOADERCONCURRENTACCESS_RESULT_SUCCESS       = 0,
    FILELOADERCONCURRENTACCESS_RESULT_NOT_FOUND     = 1,
    FILELOADERCONCURRENTACCESS_RESULT_POOL_FULL     = 2,
    FILELOADERCONCURRENTACCESS_RESULT_BLACKLISTED   = 3,
    FILELOADERCONCURRENTACCESS_RESULT_STALE_HANDLE  = 4,
    FILELOADERCONCURRENTACCESS_RESULT_TIMEOUT       = 5,
    FILELOADERCONCURRENTACCESS_RESULT_INVALID_PARAM = 6,
} FileLoaderConcurrentAccessResult;

/* ── Handle type ──────────────────────────────────────────────────────────── */

/*!
 * Opaque handle returned by addFile().
 *
 * Encoding:  handle = (pool_idx << 44) | generation
 *   pool_idx:   20 bits (supports up to 1,048,576 nodes)
 *   generation: 44 bits (supports ~17.6 trillion recycles)
 *
 * Invalid handle: 0.  Pool index 0 is reserved (never allocated).
 */
typedef uint64_t FileLoaderConcurrentAccessHandle;

/* ── File info struct ─────────────────────────────────────────────────────── */

/*!
 * Self-contained snapshot of a file's registry state.
 *
 * Returned by getFileInfo().  All fields are populated by value — the caller
 * does not need to hold any lock or hazard pointer after the call returns.
 */
typedef struct {
    bool                                in_registry;
    bool                                is_loaded;
    bool                                recently_accessed;
    FileLoaderConcurrentAccessFileState state;
    size_t                              size_bytes;
    uint32_t                            hash;
    char                                path[PATH_MAX];
} FileLoaderConcurrentAccessFileInfo;

/* ── Forward declarations ─────────────────────────────────────────────────── */

typedef struct FileLoaderConcurrentAccess FileLoaderConcurrentAccess;

/* ── Eviction callback ────────────────────────────────────────────────────── */

/*!
 * Proactive-eviction authorisation callback.
 *
 * Invoked by the registry when the node pool is exhausted and a victim must
 * be chosen.  The callback receives a snapshot of the candidate file's state
 * and returns true if the registry is allowed to evict it.
 *
 * @param path     Filesystem path of the candidate.
 * @param info     Snapshot of the candidate's current registry state.
 * @param userCtx  Opaque context from FileLoaderConcurrentAccessConfig.
 * @return true if this file may be evicted, false to protect it.
 */
typedef bool (*FileLoaderConcurrentAccessEvictionCallback)(
    const char                          *path,
    const FileLoaderConcurrentAccessFileInfo *info,
    void                                *userCtx);

/* ── VTable interface ─────────────────────────────────────────────────────── */

/*!
 * Dispatch table for FileLoaderConcurrentAccess operations.
 *
 * Every public operation routes through this vtable.  Inline wrapper functions
 * in the public header cast the opaque pointer and call the corresponding
 * function pointer.
 *
 * The getBufferTimeout and returnBuffer entries are internal — consumers use
 * the public wrappers FileLoaderConcurrentAccessGetFileContent /
 * FileLoaderConcurrentAccessReturnFile which bake in the configured default
 * timeout.
 */
typedef struct IFileLoaderConcurrentAccess {
    FileLoaderConcurrentAccessHandle (*AddFile)(
        FileLoaderConcurrentAccess *self, const char *path);
    void (*RemoveFile)(
        FileLoaderConcurrentAccess *self, const char *path);
    bool (*GetFileInfo)(
        FileLoaderConcurrentAccess *self, const char *path,
        FileLoaderConcurrentAccessFileInfo *outInfo);

    /* ── Internal: consumer-facing GetFileContent calls with default timeout */
    void *(*GetBufferTimeout)(
        FileLoaderConcurrentAccess *self,
        FileLoaderConcurrentAccessHandle handle,
        int timeoutMs, size_t *outSize);
    void (*ReturnBuffer)(
        FileLoaderConcurrentAccess *self, void *buf);
    void (*Stop)(
        FileLoaderConcurrentAccess *self);

    /* ── Query ───────────────────────────────────────────────── */
    uint32_t (*Size)(FileLoaderConcurrentAccess *self);
    uint32_t (*Capacity)(FileLoaderConcurrentAccess *self);
    uint64_t (*Heartbeat)(FileLoaderConcurrentAccess *self);

    /* ── Thread registration ─────────────────────────────────── */
    bool (*RegisterThread)(FileLoaderConcurrentAccess *self);
    void (*UnregisterThread)(FileLoaderConcurrentAccess *self);
} IFileLoaderConcurrentAccess;

/* ── Node flags ───────────────────────────────────────────────────────────── */

#define FILELOADERCONCURRENTACCESS_FLAG_NEEDS_LOAD  0x01U
#define FILELOADERCONCURRENTACCESS_FLAG_LOADING     0x02U
#define FILELOADERCONCURRENTACCESS_FLAG_IN_CACHE    0x04U

/* ── Node struct ──────────────────────────────────────────────────────────── */

/*!
 * A single file entry in the pre-allocated node pool.
 *
 * Cache-line isolation rationale:
 *  - state, buffer, flags, cache_ref_bit, reg_ref_bit each on separate cache lines.
 *  - The monitor thread writes state/buffer/flags; consumer threads read them.
 *  - Consumer threads write cache_ref_bit/reg_ref_bit (CLOCK reference bits);
 *    the monitor reads them.
 *  - This separation prevents false sharing between the two access patterns.
 *
 * The generation field is a seq-lock version counter: every time the node is
 * allocated or freed, the generation is incremented, allowing readers to detect
 * recycling without hazard pointers.
 */
typedef struct {
    /* ── Cache-line 0: state (monitor writes, consumers read) ─── */
    _Atomic uint32_t              state;         ///< FileLoaderConcurrentAccessFileState
    char                          _pad0[60];

    /* ── Cache-line 1: buffer ptr (monitor publishes, consumers read) ─── */
    _Atomic uintptr_t             buffer;         ///< mmap'd buffer ptr (0 = not loaded)
    char                          _pad1[56];

    /* ── Cache-line 2: flags (monitor sets/clears, consumers read) ─── */
    _Atomic uint32_t              flags;          ///< NEEDS_LOAD | LOADING | IN_CACHE
    char                          _pad2[60];

    /* ── Cache-line 3: cache_ref_bit (consumers set, monitor reads/clears) ─── */
    _Atomic uint32_t              cache_ref_bit;    ///< CLOCK second-chance (buffer tier)
    char                          _pad3[60];

    /* ── Cache-line 4: reg_ref_bit (consumers set, eviction reads/clears) ─── */
    _Atomic uint32_t              reg_ref_bit;      ///< CLOCK second-chance (registry tier)
    char                          _pad4[60];

    /* ── Remaining fields (cold: accessed only during alloc/free/lookup) ─── */
    _Atomic uint64_t              generation;     ///< Seq-lock counter
    _Atomic bool                  is_active;
    uint32_t                      next_free;       ///< Treiber stack link
    int                           inotify_wd;
    char                          path[];         ///< FAM — sized at init (key_width bytes)
} FileLoaderConcurrentAccessNode;

/* ── Buffer cache slot ────────────────────────────────────────────────────── */

typedef struct {
    _Atomic uintptr_t slot;              ///< packed (pool_idx << 32) | generation
    char              _pad[56];          ///< cache-line pad to 64 bytes
} FileLoaderConcurrentAccessCacheSlot;

/* ── Limbo ring entry ─────────────────────────────────────────────────────── */

typedef struct {
    uintptr_t  ptr;    ///< Buffer pointer to munmap
    size_t     size;   ///< Buffer size (for munmap)
} FileLoaderConcurrentAccessLimboEntry;

/* ── Configuration struct ─────────────────────────────────────────────────── */

/*!
 * Factory-time configuration for FileLoaderConcurrentAccess.
 *
 * All fields are optional — 0/NULL means "use the compile-time default."
 * Blacklist entries are ingested at Init() time and fixed for the lifetime
 * of the instance.
 */
typedef struct {
    size_t   max_file_size_bytes;       ///< 0 = use default (1 MiB)
    FileLoaderConcurrentAccessEvictionCallback on_evicted; ///< NULL = none
    void    *on_evicted_ctx;
    uint32_t pool_capacity;           ///< 0 = use default (4096)
    uint32_t buffer_cache_capacity;    ///< 0 = use default (64)
    uint32_t key_width;               ///< 0 = use default (256)
    uint32_t hazptr_slot_count;        ///< 0 = use default (128)
    uint32_t limbo_capacity;          ///< 0 = use default (512)
    uint32_t default_buffer_timeout_ms;  ///< 0 = use compile-time default (5000)

    /* ── Fixed blacklist (ingested at factory time) ─────────────── */
    const char **blacklist_entries;    ///< Array of paths, or NULL
    uint32_t     blacklist_entry_count;  ///< Number of entries (0 = none)
} FileLoaderConcurrentAccessConfig;

/* ── Implementation struct (opaque to consumers) ──────────────────────────── */

/*!
 * Concrete implementation of the concurrent file loader.
 *
 * Consumers embed this by value (see FileLoaderConcurrentAccessInit) or
 * allocate it themselves.  The first field is the vtable — casting to
 * IFileLoaderConcurrentAccess * is valid.
 */
struct FileLoaderConcurrentAccess {
    IFileLoaderConcurrentAccess      vtable;

    LocklessFixedWidthHashMap        file_map;
    LocklessFixedWidthHashMap        blacklist_map;

    uint8_t                         *node_pool;       ///< Byte array (FAM-sized nodes)
    _Atomic uintptr_t                node_free_stack_head;
    size_t                           node_stride;      ///< sNodeSize(key_width)

    FileLoaderConcurrentAccessCacheSlot *buffer_cache;
    _Atomic uint32_t                 cache_clock_hand;
    _Atomic uint32_t                 cache_loaded_count;

    _Atomic uintptr_t               *hazptrs;
    uint32_t                         hazptr_slot_count;

    FileLoaderConcurrentAccessLimboEntry *limbo_ring;
    _Atomic uint32_t                 limbo_write_idx;
    _Atomic uint32_t                 limbo_read_idx;
    uint32_t                         limbo_capacity;

    LocklessMpscQueue                work_queue;

    _Atomic uint64_t                 registry_clock_hand;

    size_t                           max_file_size_bytes;
    FileLoaderConcurrentAccessEvictionCallback on_evicted;
    void                            *on_evicted_ctx;

    int                              inotify_fd;
    int                              epoll_fd;
    _Atomic bool                     running;
    _Atomic uint64_t                 monitor_heartbeat;
    pthread_t                        monitor_thread;

    uint32_t                         pool_capacity;
    uint32_t                         buffer_cache_capacity;
    uint32_t                         key_width;
    uint32_t                         default_buffer_timeout_ms;
};

#endif /* UFLIB_FILE_LOADER_SERVICE_TYPE_PRIV_H */
