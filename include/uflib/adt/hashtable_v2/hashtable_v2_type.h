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
 * @file hashtable_v2_type.h
 * @brief Public type definitions for HashTableV2 — opaque-handle open-addressing
 *        hash table with optional coarse-grained rwlock.
 *
 * The HashTableV2 struct is **opaque** — its definition lives in the private
 * header (src/adt/hashtable_v2/hashtable_v2_priv.h).  Consumers see only the
 * forward declaration and the configuration struct below.
 */

#ifndef UFLIB_ADT_HASHTABLE_V2_TYPE_H
#define UFLIB_ADT_HASHTABLE_V2_TYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle — consumer never sees the struct members. */
typedef struct HashTableV2 HashTableV2;

/** Opaque item type — consumer-owned payload, never allocated or freed by the table. */
typedef void HashTableV2Item;

/**
 * @brief Key extractor callback — extracts the search key from a stored item.
 *
 * @param item_ptr  The stored item container (as originally passed to Insert).
 * @return Pointer to the key within the item (or the item itself if it IS the key).
 */
typedef const void *(*HashTableV2KeyExtractor)(HashTableV2Item *item_ptr);

/**
 * @brief Key comparator callback — compares two keys for equality.
 *
 * If NULL, the table uses memcmp for fixed-width keys (key_size > 0)
 * or strcmp for c-string keys (key_size == 0).
 *
 * @param a_ptr  First key.
 * @param b_ptr  Second key.
 * @param size   Key width (0 = c-string).
 * @return 0 if equal, non-zero otherwise.
 */
typedef int (*HashTableV2KeyComparator)(const void *a_ptr, const void *b_ptr, size_t size);

/*! Configuration for HashTableV2Create().
 *
 * All fields may be zero/NULL — sensible defaults are substituted from
 * hashtable_v2_defs.h.
 */
typedef struct HashTableV2Config {
	size_t                  capacity_hint;    ///< Initial slot count (0 → CONFIG_DEFAULT 65521)
	size_t                  max_entries;      ///< Hard ceiling on entries (0 → no limit, auto-expand)
	size_t                  key_size;         ///< Key width in bytes (0 = c-string with strcmp)
	size_t                  key_offset;       ///< offsetof(Container, key_field)
	bool                    key_is_ptr;       ///< Key accessed via pointer indirection?
	bool                    enable_locking;   ///< Use internal pthread_rwlock_t?
	const char             *name;             ///< Diagnostic label (NULL → "HashTableV2")
	HashTableV2KeyExtractor key_extractor;    ///< NULL → item IS the key (identity extractor)
	HashTableV2KeyComparator key_comparator;  ///< NULL → memcmp or strcmp depending on key_size
} HashTableV2Config;

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_ADT_HASHTABLE_V2_TYPE_H */
