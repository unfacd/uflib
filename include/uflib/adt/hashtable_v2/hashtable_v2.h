/*
 Copyright (c) 2015-2025 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file hashtable_v2.h
 * @brief HashTableV2 — Opaque-handle, open-addressing, linear-probing hash table
 *        with FNV-1a hashing, tombstone discipline, and optional coarse-grained
 *        pthread_rwlock_t locking.
 *
 * This is the **V2 successor** to the legacy `adt_hashtable` (HashTable).  V1 is
 * preserved in place at `<uflib/adt/adt_hashtable.h>` for backward compatibility.
 * New code should use HashTableV2.
 *
 * ## Key improvements over V1
 *
 * | Aspect            | V1 (adt_hashtable)              | V2 (HashTableV2)                    |
 * |-------------------|---------------------------------|-------------------------------------|
 * | Handle            | Fully-exposed struct, embeddable | Opaque pointer (`HashTableV2 *`)    |
 * | Hash function     | Custom XOR/shift (unknown avalanche) | FNV-1a (well-characterized)    |
 * | Tombstone         | Compaction-based removal         | TOMBSTONE sentinel                  |
 * | Naming            | Mixed (`AddToHash`, `HashTableInstantiate`) | Consistent `HashTableV2` prefix |
 * | Parameters        | Bare names (`hash`, `data`)      | `_ptr` suffix on pointers           |
 * | Config constants  | Bare `#define` in public header  | `CONFIG_DEFAULT_*` in `_defs.h`     |
 * | Duplicate detect  | Extracted-item pointer           | Key comparison via configurable comparator |
 * | Test coverage     | Zero                             | 30+ gtest cases + standalone stress |
 * | Design document   | None                             | 13-section compliant                |
 *
 * ## Duplicate detection semantics
 *
 * By default, HashTableV2 detects duplicates by **key value** (unlike V1 which
 * compares extracted-item pointers).  Two items with equal keys (as determined
 * by the key comparator) are considered duplicates — the second insert returns
 * the existing item without inserting.
 *
 * @code{.c}
 * // Lifecycle
 * HashTableV2Config cfg = { .key_size = sizeof(uint64_t),
 *                           .key_offset = offsetof(Session, session_id),
 *                           .enable_locking = true,
 *                           .name = "SessionsByID" };
 * HashTableV2 *ht = HashTableV2Create(&cfg);
 * if (!ht) { // handle allocation failure }
 *
 * // Insert
 * Session *s = ...;
 * HashTableV2Item *existing = HashTableV2Insert(ht, s);
 * if (existing != s) {
 *     // Another session with the same session_id was already present.
 *     // 'existing' points to the previously-stored Session.
 * }
 *
 * // Lookup
 * uint64_t key = 42;
 * Session *found = (Session *)HashTableV2Lookup(ht, &key);
 *
 * // Remove
 * HashTableV2Remove(ht, s);
 *
 * // External locking (when the consumer needs atomic multi-operation sequences)
 * HashTableV2WriteLock(ht);
 * Session *s1 = (Session *)HashTableV2Lookup(ht, &key1);
 * Session *s2 = (Session *)HashTableV2Lookup(ht, &key2);
 * // ... atomic read-modify-write across both lookups ...
 * HashTableV2Unlock(ht);
 *
 * // Teardown
 * HashTableV2Destroy(ht);
 * @endcode
 */

#ifndef UFLIB_ADT_HASHTABLE_V2_H
#define UFLIB_ADT_HASHTABLE_V2_H

#include <uflib/uflib_defs.h>

#include <stdbool.h>
#include <stddef.h>

#include <uflib/adt/hashtable_v2/hashtable_v2_type.h>

/* ──────────────────────────────────────────────
 * Lifecycle
 * ────────────────────────────────────────────── */

/**
 * @brief Create a new HashTableV2 instance.
 *
 * @param config_ptr  Configuration (NULL → all defaults from hashtable_v2_defs.h).
 * @return Opaque handle, or NULL on allocation failure.
 *
 * @code{.c}
 * HashTableV2Config cfg = { .key_size = 8, .enable_locking = true };
 * HashTableV2 *ht = HashTableV2Create(&cfg);
 * @endcode
 */
PUBLIC_API HashTableV2 *
HashTableV2Create(const HashTableV2Config *config_ptr);

/**
 * @brief Destroy a HashTableV2 instance, freeing all internal memory.
 *
 * NULL-safe — HashTableV2Destroy(NULL) is a no-op.
 * Does NOT free the stored items (they are caller-owned).
 *
 * @param ht_ptr  Handle returned by HashTableV2Create().
 */
PUBLIC_API void
HashTableV2Destroy(HashTableV2 *ht_ptr);

/* ──────────────────────────────────────────────
 * Core Operations
 * ────────────────────────────────────────────── */

/**
 * @brief Insert an item into the hash table.
 *
 * If an item with an equal key is already present, it is NOT replaced —
 * the existing item is returned and \p is_added_ptr (if non-NULL) is set
 * to `true`.
 *
 * The table auto-expands at 66% load factor (configurable via _defs.h).
 *
 * @param ht_ptr        Handle.
 * @param item_ptr       Item to insert (caller-owned, never freed by the table).
 * @param is_added_ptr   Optional: set to true if item was already present.
 * @return The stored item (item_ptr if newly inserted, or the pre-existing item).
 */
PUBLIC_API HashTableV2Item *
HashTableV2Insert(HashTableV2 *ht_ptr, HashTableV2Item *item_ptr,
                  bool *is_added_ptr);

/**
 * @brief Look up an item by key.
 *
 * @param ht_ptr    Handle.
 * @param key_ptr   Key to search for.
 * @return The stored item, or NULL if not found.
 */
PUBLIC_API HashTableV2Item *
HashTableV2Lookup(HashTableV2 *ht_ptr, const void *key_ptr);

/**
 * @brief Remove an item from the hash table.
 *
 * The slot is marked TOMBSTONE (preserving probe chains).  The removed
 * item is returned to the caller — it is the caller's responsibility to
 * free it if appropriate.
 *
 * @param ht_ptr    Handle.
 * @param item_ptr   Item to remove (must be the exact pointer returned by Insert/Lookup).
 * @return The removed item, or NULL if not found.
 */
PUBLIC_API HashTableV2Item *
HashTableV2Remove(HashTableV2 *ht_ptr, HashTableV2Item *item_ptr);

/* ──────────────────────────────────────────────
 * Query
 * ────────────────────────────────────────────── */

/**
 * @brief Return the current number of entries.
 *
 * @param ht_ptr  Handle.
 * @return Entry count (snapshot — may be stale under concurrent access).
 */
PUBLIC_API size_t
HashTableV2Size(HashTableV2 *ht_ptr);

/**
 * @brief Return the physical slot capacity.
 *
 * @param ht_ptr  Handle.
 * @return Total number of slots (empty + occupied + tombstone).
 */
PUBLIC_API size_t
HashTableV2Capacity(HashTableV2 *ht_ptr);

/* ──────────────────────────────────────────────
 * Bulk Operations
 * ────────────────────────────────────────────── */

/**
 * @brief Enumerate all items into a caller-provided array.
 *
 * @param ht_ptr           Handle.
 * @param out_array_ptr     Pre-allocated array of HashTableV2Item* pointers.
 * @param array_capacity    Maximum number of items the array can hold.
 * @return Number of items written to out_array_ptr, or -1 on error.
 *
 * @code{.c}
 * HashTableV2Item *items[1024];
 * long n = HashTableV2Enumerate(ht, items, 1024);
 * for (long i = 0; i < n; i++) { // process items[i] }
 * @endcode
 */
PUBLIC_API long
HashTableV2Enumerate(HashTableV2 *ht_ptr, HashTableV2Item **out_array_ptr,
                     long array_capacity);

/**
 * @brief Merge all entries from source into destination.
 *
 * Items present in both tables are NOT overwritten — the destination's
 * copy is preserved.
 *
 * @param dest_ptr   Destination table.
 * @param src_ptr    Source table (not modified).
 */
PUBLIC_API void
HashTableV2Merge(HashTableV2 *dest_ptr, HashTableV2 *src_ptr);

/* ──────────────────────────────────────────────
 * External Locking
 * ────────────────────────────────────────────── */

/**
 * @brief Acquire the internal read (shared) lock.
 *
 * Multiple readers may hold the lock concurrently.  Writers are excluded.
 * No-op if enable_locking was false at creation.
 *
 * @param ht_ptr    Handle.
 * @param try_flag   If non-zero, use tryrdlock (non-blocking).
 * @return 0 on success, non-zero if the lock could not be acquired.
 */
PUBLIC_API int
HashTableV2ReadLock(HashTableV2 *ht_ptr, int try_flag);

/**
 * @brief Acquire the internal write (exclusive) lock.
 *
 * Only one writer may hold the lock.  Readers and other writers are excluded.
 * No-op if enable_locking was false at creation.
 *
 * @param ht_ptr    Handle.
 * @param try_flag   If non-zero, use trywrlock (non-blocking).
 * @return 0 on success, non-zero if the lock could not be acquired.
 */
PUBLIC_API int
HashTableV2WriteLock(HashTableV2 *ht_ptr, int try_flag);

/**
 * @brief Release the internal lock (read or write).
 *
 * No-op if enable_locking was false at creation.
 *
 * @param ht_ptr  Handle.
 * @return 0 on success, non-zero on error.
 */
PUBLIC_API int
HashTableV2Unlock(HashTableV2 *ht_ptr);

#endif /* UFLIB_ADT_HASHTABLE_V2_H */
