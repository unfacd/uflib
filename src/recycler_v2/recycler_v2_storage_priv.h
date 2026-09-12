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
 * @file recycler_v2_storage_priv.h
 * @brief RecyclerV2 storage layer — internal API for marshalled blob persistence
 *
 * Each marshaller_id is unique (monotonic counter per type pool).
 * Concurrent reads/writes to the same ID are impossible.  Per-type directories
 * are created once at init.  No locking needed in the storage layer.
 */

#ifndef UFLIB_RECYCLER_V2_RECYCLER_V2_STORAGE_PRIV_H
#define UFLIB_RECYCLER_V2_RECYCLER_V2_STORAGE_PRIV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <uflib/recycler_v2/recycler_v2_defs.h>
#include <uflib/recycler_v2/recycler_v2_type.h>

/* Forward declaration — full definition in recycler_v2_priv.h */
typedef struct RecyclerV2PoolDefinition RecyclerV2PoolDefinition;

/* ── Storage API (recycler-internal only) ──────────────────────────────────── */

/**
 * @brief Initialize storage for a type pool.
 *
 * Handles stale blobs from prior runs according to @p policy:
 *   OVERWRITE — recursively delete <storage_path>/*, then recreate
 *   APPEND    — ensure directory exists (keep old blobs — they're orphaned)
 *   ARCHIVE   — move <storage_path> to <root>/archived/<type>_<timestamp>,
 *               then recreate <storage_path>
 *
 * @param pool_ptr  Pool definition.
 * @param policy    Stale-blob handling policy.
 * @return 0 on success, -1 on failure.
 */
int
RecyclerV2StorageInit(RecyclerV2PoolDefinition *pool_ptr,
                      RecyclerV2StorageInitPolicy policy);

/**
 * @brief Tear down storage for a type pool.
 *
 * Does NOT delete marshalled blobs — only removes empty directories.
 * Call after all marshalled holders have been resolved.
 *
 * @param pool_ptr  Pool definition.
 */
void
RecyclerV2StorageDestroy(RecyclerV2PoolDefinition *pool_ptr);

/**
 * @brief Write a marshalled blob to disk.
 *
 * File layout: <storage_root>/<type_name>/<marshaller_id_hex>.pb
 * On failure (disk full, permissions), logs the error and returns -1.
 * The caller (sMarshalVictim) aborts the marshal on write failure.
 *
 * @param pool_ptr        Pool definition.
 * @param data_ptr        Serialized bytes to write.
 * @param len             Number of bytes.
 * @param out_id_ptr      Receives the allocated marshaller_id.
 * @return 0 on success, -1 on failure.
 */
int
RecyclerV2StorageWrite(RecyclerV2PoolDefinition *pool_ptr,
                       const uint8_t *data_ptr, size_t len,
                       uint64_t *out_id_ptr);

/**
 * @brief Read a marshalled blob from disk.
 *
 * Allocates memory for the blob (caller must free via free()).
 *
 * @param pool_ptr     Pool definition.
 * @param id           Marshaller ID to read.
 * @param out_data_ptr Receives pointer to allocated buffer (caller must free).
 * @param out_len_ptr  Receives number of bytes read.
 * @return 0 on success, -1 on failure (file not found, read error, corrupted).
 */
int
RecyclerV2StorageRead(RecyclerV2PoolDefinition *pool_ptr,
                      uint64_t id,
                      uint8_t **out_data_ptr, size_t *out_len_ptr);

/**
 * @brief Delete a marshalled blob from disk.
 *
 * Best-effort — failure is logged but not propagated (the blob is orphaned
 * on disk, which is a slow leak, not a correctness issue).
 *
 * @param pool_ptr  Pool definition.
 * @param id        Marshaller ID to delete.
 */
void
RecyclerV2StorageDelete(RecyclerV2PoolDefinition *pool_ptr, uint64_t id);

/**
 * @brief Check if the storage layer has space available.
 *
 * Simple check: attempts to create a sentinel file and immediately deletes it.
 * Returns false if the filesystem is read-only or out of space.
 *
 * @param pool_ptr  Pool definition.
 * @return true if writes are likely to succeed, false otherwise.
 */
bool
RecyclerV2StorageHasSpace(RecyclerV2PoolDefinition *pool_ptr);

#endif /* UFLIB_RECYCLER_V2_RECYCLER_V2_STORAGE_PRIV_H */
