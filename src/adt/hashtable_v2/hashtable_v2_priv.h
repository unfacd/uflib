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
 * @file hashtable_v2_priv.h
 * @brief Private type definitions for HashTableV2 — NOT INSTALLED.
 *
 * This header is colocated with hashtable_v2.c and is visible only to:
 *  - hashtable_v2.c (the implementation)
 *  - hashtable_v2_tests.cpp (white-box unit tests)
 *
 * Consumers MUST NOT include this file — the HashTableV2 struct is opaque
 * in the public API (<uflib/adt/hashtable_v2/hashtable_v2_type.h>).
 */

#ifndef UFLIB_SRC_ADT_HASHTABLE_V2_PRIV_H
#define UFLIB_SRC_ADT_HASHTABLE_V2_PRIV_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/adt/hashtable_v2/hashtable_v2_type.h>
#include <uflib/adt/hashtable_v2/hashtable_v2_defs.h>
#include <uflib/logger/logger_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────
 * Internal slot type
 * ────────────────────────────────────────────── */

/**
 * @brief A single slot in the open-addressing hash table.
 *
 * Each slot is either EMPTY (NULL), TOMBSTONE ((void *)1), or OCCUPIED
 * (points to a caller-owned item).
 */
typedef HashTableV2Item *HashTableV2Slot;

/* ──────────────────────────────────────────────
 * Private HashTableV2 struct (opaque to consumers)
 * ────────────────────────────────────────────── */

struct HashTableV2 {
	HashTableV2Slot        *slots;             /**< Slot array (heap-allocated, size = capacity) */
	size_t                  capacity;           /**< Physical slot count (always > 0 after create) */
	size_t                  num_entries;        /**< Current occupied (non-TOMBSTONE) count */
	size_t                  max_entries;        /**< Hard ceiling: 0=unlimited, >0=reject insert when full */
	size_t                  key_size;           /**< Key width: 0=c-string, >0=fixed-width */
	size_t                  key_offset;         /**< offsetof(Container, key_field) */
	bool                    key_is_ptr;         /**< Key accessed via pointer indirection? */
	bool                    enable_locking;     /**< Use internal pthread_rwlock_t? */
	char                   *name;              /**< Diagnostic label (heap-allocated copy) */
	HashTableV2KeyExtractor key_extractor;      /**< NULL → identity (item IS the key) */
	HashTableV2KeyComparator key_comparator;    /**< NULL → memcmp/strcmp based on key_size */
	pthread_rwlock_t        rwlock;             /**< Coarse-grained lock (only if enable_locking) */
	UfLogger               *uf_logger;          /**< Borrowed diagnostic sink (write-once); NULL = silent. */
};

/* ──────────────────────────────────────────────
 * Internal helpers (declared for test white-box access)
 * ────────────────────────────────────────────── */

/**
 * @brief FNV-1a hash of a key, reduced modulo table_size.
 *
 * @param key_ptr    Pointer to key bytes.
 * @param key_len    Length in bytes (0 → use strlen).
 * @param table_size Divisor (table capacity, always > 0).
 * @return Slot index in [0, table_size).
 */
uint32_t
sHashFn(const void *key_ptr, size_t key_len, size_t table_size);

/**
 * @brief Extract the key pointer from a stored item.
 *
 * Uses key_offset and key_is_ptr from the config.
 */
const void *
sExtractKey(HashTableV2 *ht_ptr, HashTableV2Item *item_ptr);

/**
 * @brief Compare two keys for equality.
 *
 * Uses the configured comparator, or falls back to memcmp/strcmp.
 */
bool
sKeyEqual(HashTableV2 *ht_ptr, const void *a_ptr, const void *b_ptr);

/**
 * @brief Return the next prime >= x (used for table expansion).
 */
size_t
sNextPrime(size_t x);

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_SRC_ADT_HASHTABLE_V2_PRIV_H */
