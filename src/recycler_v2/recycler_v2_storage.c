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
 * @file recycler_v2_storage.c
 * @brief RecyclerV2 storage layer — filesystem-backed marshalled blob persistence
 *
 * Layout: <CONFIG_DEFAULT_RECYCLER_V2_STORAGE_ROOT>/<type_name>/<id_hex>.pb
 *
 * Each marshaller_id is unique (monotonic counter per type pool).
 * Concurrent reads/writes to the same ID are impossible.
 * Per-type directories are created once at init.  No locking needed.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include "recycler_v2_storage_priv.h"
#include "recycler_v2_priv.h"

#include <uflib/recycler_v2/recycler_v2.h>
#include <uflib/recycler_v2/recycler_v2_defs.h>
#include "recycler_v2_log_strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

/* ── Helper: build file path ──────────────────────────────────────────────── */

/**
 * @brief Build the full filesystem path for a marshalled blob.
 *
 * Format: <storage_root>/<type_name>/<marshaller_id_hex>.pb
 *
 * @param pool_ptr  Pool definition.
 * @param id        Marshaller ID.
 * @param buf       Output buffer.
 * @param buf_sz    Size of output buffer.
 * @return 0 on success, -1 if buffer too small.
 */
static int
sStorageBuildPath(RecyclerV2PoolDefinition *pool_ptr, uint64_t id,
                  char *buf, size_t buf_sz)
{
    int needed = snprintf(buf, buf_sz, "%s/%016lx.pb",
        pool_ptr->storage_path, (unsigned long)id);
    if (needed < 0 || (size_t)needed >= buf_sz) return -1;
    return 0;
}

/* ── Public: Init / Destroy ───────────────────────────────────────────────── */

/**
 * @brief Create a directory and all parent directories (like mkdir -p).
 *
 * @param path  Full path to create.
 * @param mode  Permissions for leaf directory.
 * @return 0 on success, -1 on failure.
 */
static int
sMkdirRecursive(const char *path, mode_t mode)
{
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;

    memcpy(tmp, path, len + 1);

    /* Walk path creating each component */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }

    /* Create leaf directory with specified mode */
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

/* ── Helper: recursively delete a directory tree ──────────────────────────── */

static int
sRmRecursive(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return (errno == ENOENT) ? 0 : -1;

    struct dirent *entry;
    char sub[1024];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(sub, sizeof(sub), "%s/%s", path, entry->d_name);
        struct stat st;
        if (lstat(sub, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (sRmRecursive(sub) != 0) { closedir(dir); return -1; }
        } else {
            if (unlink(sub) != 0 && errno != ENOENT) {
                closedir(dir);
                return -1;
            }
        }
    }
    closedir(dir);
    rmdir(path);
    return 0;
}

/* ── Helper: move a directory tree ─────────────────────────────────────────── */

static int
sMoveDirectory(const char *src, const char *dst)
{
    if (rename(src, dst) == 0) return 0;
    if (errno != EXDEV) return -1;  // cross-device not supported

    /* Cross-device: recursive copy then delete (not implemented —
     * storage root should always be on a single filesystem). */
    return -1;
}

int
RecyclerV2StorageInit(RecyclerV2PoolDefinition *pool_ptr,
                      RecyclerV2StorageInitPolicy policy)
{
    if (!pool_ptr || !pool_ptr->storage_path[0]) return -1;

    struct stat st;
    bool exists = (stat(pool_ptr->storage_path, &st) == 0 && S_ISDIR(st.st_mode));

    switch (policy) {

    case RECYCLER_V2_STORAGE_OVERWRITE:
        /* Delete old blobs — marshaller IDs restart from 1 on each
         * process lifetime, so old blobs can never be resolved. */
        if (exists) {
            if (sRmRecursive(pool_ptr->storage_path) != 0) {
                syslog(LOG_ERR, "%s: OVERWRITE — failed to remove '%s': %s",
                    __func__, pool_ptr->storage_path, strerror(errno));
                return -1;
            }
        }
        break;

    case RECYCLER_V2_STORAGE_APPEND:
        /* Keep old blobs.  They'll be orphaned (IDs are monotonic and
         * never repeat within a process lifetime) but the consumer may
         * want them for debugging.  Nothing to do — directory already
         * exists (or will be created below). */
        break;

    case RECYCLER_V2_STORAGE_ARCHIVE:
        /* Move existing blobs to <root>/archived/<type>_<timestamp>/.
         * The archived directory preserves the prior run's state for
         * post-mortem analysis. */
        if (exists) {
            /* Extract type_name from storage_path (last component).
             * storage_path = <root>/<type_name> */
            const char *type_name = strrchr(pool_ptr->storage_path, '/');
            type_name = type_name ? type_name + 1 : pool_ptr->storage_path;

            /* Build archive path */
            time_t now = time(NULL);
            char archive_path[1024];
            snprintf(archive_path, sizeof(archive_path),
                "%s/archived/%s_%ld",
                pool_ptr->storage_root_ptr, type_name, (long)now);

            /* Ensure archived/ directory exists */
            if (sMkdirRecursive(archive_path, 0700) != 0) {
                syslog(LOG_ERR, "%s: ARCHIVE — mkdir '%s' failed: %s",
                    __func__, archive_path, strerror(errno));
                return -1;
            }
            /* Remove the archive dir itself — rename() needs the target
             * to NOT exist, and we want <storage_path> renamed TO
             * <archive_path>.  So: create archived/ parent, then rename
             * the type dir into it. */
            rmdir(archive_path);  // remove leaf, rename will recreate it

            if (sMoveDirectory(pool_ptr->storage_path, archive_path) != 0) {
                syslog(LOG_ERR, "%s: ARCHIVE — rename '%s' → '%s' failed: %s",
                    __func__, pool_ptr->storage_path, archive_path,
                    strerror(errno));
                return -1;
            }
        }
        break;
    }

    /* Create the per-type directory tree (rwx------) */
    if (sMkdirRecursive(pool_ptr->storage_path, 0700) != 0) {
        syslog(LOG_ERR, "%s: mkdir '%s' failed: %s", __func__,
            pool_ptr->storage_path, strerror(errno));
        return -1;
    }

    return 0;
}

void
RecyclerV2StorageDestroy(RecyclerV2PoolDefinition *pool_ptr)
{
    if (!pool_ptr || !pool_ptr->storage_path[0]) return;

    /* Best-effort: remove the directory if empty.
     * We do NOT recursively delete — existing marshalled blobs are
     * the consumer's responsibility to resolve before shutdown. */
    rmdir(pool_ptr->storage_path);
}

/* ── Public: Write ────────────────────────────────────────────────────────── */

int
RecyclerV2StorageWrite(RecyclerV2PoolDefinition *pool_ptr,
                       const uint8_t *data_ptr, size_t len,
                       uint64_t *out_id_ptr)
{
    if (!pool_ptr || !data_ptr || !out_id_ptr) return -1;

    /* Allocate a unique ID */
    uint64_t id = atomic_fetch_add_explicit(&pool_ptr->next_marshaller_id, 1,
                                            memory_order_relaxed);
    if (id == 0 || id > RECYCLER_V2_MARSHALLER_ID_MAX) {
        return -1;
    }

    /* Build file path */
    char path[1024];
    if (sStorageBuildPath(pool_ptr, id, path, sizeof(path)) != 0) {
        return -1;
    }

    /* Write file atomically: write to temp, then rename */
    char tmp_path[1080];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, getpid());

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        syslog(LOG_ERR, LOGSTR_RECYCLER_V2_STORAGE_WRITE_FAILED,
            __func__, (unsigned long)id, strerror(errno));
        return -1;
    }

    ssize_t written = write(fd, data_ptr, len);
    if (written < 0 || (size_t)written != len) {
        syslog(LOG_ERR, LOGSTR_RECYCLER_V2_STORAGE_WRITE_FAILED,
            __func__, (unsigned long)id, strerror(errno));
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    /* Ensure data is on disk before rename */
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    /* Atomic rename */
    if (rename(tmp_path, path) != 0) {
        syslog(LOG_ERR, LOGSTR_RECYCLER_V2_STORAGE_WRITE_FAILED,
            __func__, (unsigned long)id, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    /* Persist the directory entry as well as the file contents. */
#ifdef O_DIRECTORY
    int dirfd = open(pool_ptr->storage_path, O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) {
        (void)fsync(dirfd);
        close(dirfd);
    }
#endif

    *out_id_ptr = id;
    return 0;
}

/* ── Public: Read ─────────────────────────────────────────────────────────── */

int
RecyclerV2StorageRead(RecyclerV2PoolDefinition *pool_ptr,
                      uint64_t id,
                      uint8_t **out_data_ptr, size_t *out_len_ptr)
{
    if (!pool_ptr || !out_data_ptr || !out_len_ptr) return -1;

    char path[1024];
    if (sStorageBuildPath(pool_ptr, id, path, sizeof(path)) != 0) {
        return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        syslog(LOG_ERR, LOGSTR_RECYCLER_V2_STORAGE_READ_FAILED,
            __func__, (unsigned long)id, strerror(errno));
        return -1;
    }

    if (st.st_size == 0) {
        *out_data_ptr = NULL;
        *out_len_ptr = 0;
        return 0;
    }

    /* Sanity: refuse to allocate more than 16 MiB for a single blob */
    if (st.st_size > 16 * 1024 * 1024) {
        syslog(LOG_ERR, LOGSTR_RECYCLER_V2_STORAGE_READ_FAILED,
            __func__, (unsigned long)id, "blob too large");
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)st.st_size);
    if (!buf) return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        syslog(LOG_ERR, LOGSTR_RECYCLER_V2_STORAGE_READ_FAILED,
            __func__, (unsigned long)id, strerror(errno));
        free(buf);
        return -1;
    }

    ssize_t nread = read(fd, buf, (size_t)st.st_size);
    close(fd);

    if (nread != st.st_size) {
        syslog(LOG_ERR, LOGSTR_RECYCLER_V2_STORAGE_READ_FAILED,
            __func__, (unsigned long)id,
            nread < 0 ? strerror(errno) : "partial read");
        free(buf);
        return -1;
    }

    *out_data_ptr = buf;
    *out_len_ptr = (size_t)nread;
    return 0;
}

/* ── Public: Delete ───────────────────────────────────────────────────────── */

void
RecyclerV2StorageDelete(RecyclerV2PoolDefinition *pool_ptr, uint64_t id)
{
    if (!pool_ptr) return;

    char path[1024];
    if (sStorageBuildPath(pool_ptr, id, path, sizeof(path)) != 0) {
        return;
    }

    if (unlink(path) != 0 && errno != ENOENT) {
        syslog(LOG_ERR, LOGSTR_RECYCLER_V2_STORAGE_DELETE_FAILED,
            __func__, (unsigned long)id, strerror(errno));
    }
}

/* ── Public: Space check ──────────────────────────────────────────────────── */

bool
RecyclerV2StorageHasSpace(RecyclerV2PoolDefinition *pool_ptr)
{
    if (!pool_ptr || !pool_ptr->storage_path[0]) return false;

    /* Write a sentinel file and immediately delete it */
    char sentinel[1100];
    snprintf(sentinel, sizeof(sentinel), "%s/.space_check_%d",
        pool_ptr->storage_path, getpid());

    int fd = open(sentinel, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (write(fd, "x", 1) != 1) {
        close(fd);
        unlink(sentinel);
        return false;
    }
    close(fd);
    unlink(sentinel);
    return true;
}
