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
 * @file hashtable_v2.c
 * @brief HashTableV2 implementation — FNV-1a hashing, open-addressing with
 *        linear probing, TOMBSTONE discipline, and optional coarse-grained
 *        pthread_rwlock_t locking.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include "hashtable_v2_priv.h"

#include <stdlib.h>
#include <string.h>
#include <uflib/main_types.h>
#include <uflib/utils.h>

/* Forward declarations — locking functions are defined after their call sites */
static int  sReadLock(HashTableV2 *ht_ptr, int try_flag);
static int  sWriteLock(HashTableV2 *ht_ptr, int try_flag);
static int  sUnlock(HashTableV2 *ht_ptr);

/* ═══════════════════════════════════════════════════════════════════
 * Internal helpers
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief FNV-1a hash of a key, reduced modulo table_size.
 *
 * Uses the same FNV-1a construction as LocklessFixedWidthHashMap::sHashTruncated
 * (see UF_LIBRARY_CONVENTIONS.md §6).  For c-string keys (key_len == 0), stops at
 * NUL.  For fixed-width keys, hashes exactly key_len bytes.
 *
 * @param key_ptr    Pointer to key bytes.
 * @param key_len    Length in bytes (0 → use strlen, stop at NUL).
 * @param table_size Divisor (always > 0).
 * @return Slot index in [0, table_size).
 */
uint32_t
sHashFn(const void *key_ptr, size_t key_len, size_t table_size)
{
	const unsigned char *p = (const unsigned char *)key_ptr;
	uint32_t hash = 2166136261U;   /* FNV offset basis */
	size_t i = 0;

	if (key_len == 0) {
		/* c-string — hash until NUL */
		while (*p) {
			hash ^= *p++;
			hash *= 16777619U;   /* FNV prime */
			i++;
		}
	} else {
		while (i < key_len) {
			hash ^= p[i];
			hash *= 16777619U;
			i++;
		}
	}

	return hash % table_size;
}

/**
 * @brief Extract the key pointer from a stored item.
 */
const void *
sExtractKey(HashTableV2 *ht_ptr, HashTableV2Item *item_ptr)
{
	/* If a custom extractor is provided, use it */
	if (IS_PRESENT(ht_ptr->key_extractor)) {
		return ht_ptr->key_extractor(item_ptr);
	}

	/* No custom extractor — use offset-based extraction */
	if (ht_ptr->key_is_ptr) {
		return *(const void **)((const char *)item_ptr + ht_ptr->key_offset);
	}

	if (ht_ptr->key_offset != 0 || ht_ptr->key_size != 0) {
		return (const char *)item_ptr + ht_ptr->key_offset;
	}

	/* Identity — the item itself is the key */
	return item_ptr;
}

/**
 * @brief Compare two keys for equality.
 */
bool
sKeyEqual(HashTableV2 *ht_ptr, const void *a_ptr, const void *b_ptr)
{
	if (IS_PRESENT(ht_ptr->key_comparator)) {
		return ht_ptr->key_comparator(a_ptr, b_ptr, ht_ptr->key_size) == 0;
	}

	/* Default comparator */
	if (ht_ptr->key_size == 0) {
		return strcmp((const char *)a_ptr, (const char *)b_ptr) == 0;
	}

	return memcmp(a_ptr, b_ptr, ht_ptr->key_size) == 0;
}

/**
 * @brief Is a slot available for a new insertion?
 *
 * EMPTY and TOMBSTONE slots are both claimable.  OCCUPIED slots are not.
 */
static inline bool
sSlotIsClaimable(HashTableV2Slot *slot_ptr)
{
	return (IS_EMPTY(*slot_ptr) || *slot_ptr == PRIV_HASHTABLE_V2_SLOT_TOMBSTONE);
}

/**
 * @brief Return the next prime >= x.
 *
 * Uses a simple trial-division sieve.  For the sizes encountered in hash-table
 * expansion (64 Ki – ~8 Mi), this is fast enough.
 */
size_t
sNextPrime(size_t x)
{
	/* Pre-computed primes for common bootstrap sizes */
	static const size_t kPrimes[] = {
		7, 13, 31, 61, 127, 251, 509, 1021, 2039, 4093, 8191, 16381,
		32771, 65521, 131071, 262147, 524287, 1048573, 2097143,
		4194301, 8388593, 16777213, 33554393, 67108859, 134217689
	};
	const size_t nPrimes = sizeof(kPrimes) / sizeof(kPrimes[0]);

	for (size_t i = 0; i < nPrimes; i++) {
		if (kPrimes[i] >= x) return kPrimes[i];
	}

	/* Fallback: trial division for values beyond the table */
	if (x % 2 == 0) x++;
	while (1) {
		int is_prime = 1;
		for (size_t d = 3; d * d <= x; d += 2) {
			if (x % d == 0) { is_prime = 0; break; }
		}
		if (is_prime) return x;
		x += 2;
	}
}

/**
 * @brief Expand the hash table to the next prime >= 2× current capacity.
 *
 * All existing items are rehashed into the new slot array.  Tombstones
 * are naturally dropped (they are not items and not re-inserted).
 */
static void
sExpandTable(HashTableV2 *ht_ptr)
{
	size_t old_capacity = ht_ptr->capacity;

	/* Guard against overflow */
	if (old_capacity > SIZE_MAX / 2) return;

	size_t new_capacity = sNextPrime(old_capacity * 2);
	HashTableV2Slot *new_slots = (HashTableV2Slot *)calloc(new_capacity,
	                                                       sizeof(HashTableV2Slot));
	if (IS_EMPTY(new_slots)) return;

	/* Rehash all occupied slots */
	for (size_t i = 0; i < old_capacity; i++) {
		HashTableV2Slot *slot = &ht_ptr->slots[i];
		if (IS_EMPTY(*slot) || *slot == PRIV_HASHTABLE_V2_SLOT_TOMBSTONE)
			continue;

		const void *key_ptr = sExtractKey(ht_ptr, *slot);
		size_t key_len = ht_ptr->key_size;
		uint32_t h = sHashFn(key_ptr, key_len, new_capacity);

		/* Linear probe to first claimable slot */
		while (!IS_EMPTY(new_slots[h]))
			h = (h + 1) % new_capacity;

		new_slots[h] = *slot;
	}

	free(ht_ptr->slots);
	ht_ptr->slots    = new_slots;
	ht_ptr->capacity = new_capacity;
}

/**
 * @brief Find the slot index for a given key, or the first claimable slot
 *        (if inserting).
 *
 * @param ht_ptr        Handle.
 * @param key_ptr       Key to search for.
 * @param insert_mode   If true, return the first claimable slot when key not found.
 * @param[out] out_slot_ptr  Set to the slot pointer if found/claimable.
 * @return The slot index, or capacity if the table is full (insert only).
 */
static size_t
sFindSlot(HashTableV2 *ht_ptr, const void *key_ptr, bool insert_mode,
          HashTableV2Slot **out_slot_ptr)
{
	size_t key_len = ht_ptr->key_size;
	uint32_t h = sHashFn(key_ptr, key_len, ht_ptr->capacity);
	size_t start = h;
	size_t first_tombstone = ht_ptr->capacity;  /* sentinel = not found */
	HashTableV2Slot *slot;

	for (;;) {
		slot = &ht_ptr->slots[h];

		if (IS_EMPTY(*slot)) {
			/* Key not found.  If inserting, claim this slot (or an earlier tombstone). */
			if (insert_mode) {
				if (first_tombstone != ht_ptr->capacity) {
					*out_slot_ptr = &ht_ptr->slots[first_tombstone];
					return first_tombstone;
				}
				*out_slot_ptr = slot;
				return h;
			}
			*out_slot_ptr = NULL;
			return ht_ptr->capacity;
		}

		if (*slot == PRIV_HASHTABLE_V2_SLOT_TOMBSTONE) {
			/* Remember the first tombstone for potential insertion */
			if (insert_mode && first_tombstone == ht_ptr->capacity)
				first_tombstone = h;
		} else {
			/* Occupied slot — compare keys */
			const void *stored_key = sExtractKey(ht_ptr, *slot);
			if (sKeyEqual(ht_ptr, stored_key, key_ptr)) {
				*out_slot_ptr = slot;
				return h;
			}
		}

		h = (h + 1) % ht_ptr->capacity;

		/* Full cycle — table is full (only possible in insert mode with no
		 * tombstones or empty slots). */
		if (h == start) {
			if (insert_mode && first_tombstone != ht_ptr->capacity) {
				*out_slot_ptr = &ht_ptr->slots[first_tombstone];
				return first_tombstone;
			}
			*out_slot_ptr = NULL;
			return ht_ptr->capacity;
		}
	}
}

/* ═══════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

PUBLIC_API HashTableV2 *
HashTableV2Create(const HashTableV2Config *config_ptr)
{
	HashTableV2 *ht_ptr;
	HashTableV2Config cfg;
	size_t capacity;

	/* Default config */
	if (IS_PRESENT(config_ptr)) {
		cfg = *config_ptr;
	} else {
		memset(&cfg, 0, sizeof(cfg));
	}

	capacity = cfg.capacity_hint > 0
	           ? sNextPrime(cfg.capacity_hint)
	           : CONFIG_DEFAULT_HASHTABLE_V2_INITIAL_SIZE;

	ht_ptr = (HashTableV2 *)calloc(1, sizeof(HashTableV2));
	if (IS_EMPTY(ht_ptr)) return NULL;

	ht_ptr->slots = (HashTableV2Slot *)calloc(capacity, sizeof(HashTableV2Slot));
	if (IS_EMPTY(ht_ptr->slots)) {
		free(ht_ptr);
		return NULL;
	}

	ht_ptr->capacity       = capacity;
	ht_ptr->num_entries    = 0;
	ht_ptr->max_entries    = cfg.max_entries;
	ht_ptr->key_size       = cfg.key_size;
	ht_ptr->key_offset     = cfg.key_offset;
	ht_ptr->key_is_ptr     = cfg.key_is_ptr;
	ht_ptr->enable_locking = cfg.enable_locking;
	ht_ptr->name           = cfg.name ? strdup(cfg.name)
	                                  : strdup("HashTableV2");
	ht_ptr->key_extractor  = cfg.key_extractor;
	ht_ptr->key_comparator = cfg.key_comparator;

	if (ht_ptr->enable_locking) {
		int ret = pthread_rwlock_init(&ht_ptr->rwlock, NULL);
		if (ret != 0) {
			free(ht_ptr->slots);
			free(ht_ptr->name);
			free(ht_ptr);
			return NULL;
		}
	}

	return ht_ptr;
}

PUBLIC_API void
HashTableV2Destroy(HashTableV2 *ht_ptr)
{
	if (IS_EMPTY(ht_ptr)) return;

	if (ht_ptr->enable_locking)
		pthread_rwlock_destroy(&ht_ptr->rwlock);

	free(ht_ptr->slots);
	free(ht_ptr->name);
	free(ht_ptr);
}

/* ═══════════════════════════════════════════════════════════════════
 * Core Operations
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Check if the table needs expansion (≥ 2/3 load factor).
 */
static inline bool
sNeedsExpand(HashTableV2 *ht_ptr)
{
	/* Hard ceiling — reject further inserts, never expand */
	if (ht_ptr->max_entries > 0 && ht_ptr->num_entries >= ht_ptr->max_entries)
		return false;

	return (ht_ptr->num_entries * PRIV_CONFIG_DEFAULT_HASHTABLE_V2_LOAD_FACTOR_DEN
	        >= ht_ptr->capacity * PRIV_CONFIG_DEFAULT_HASHTABLE_V2_LOAD_FACTOR_NUM);
}

/**
 * @brief Check if the table is at its hard capacity ceiling.
 */
static inline bool
sAtCapacity(HashTableV2 *ht_ptr)
{
	return (ht_ptr->max_entries > 0 && ht_ptr->num_entries >= ht_ptr->max_entries);
}

PUBLIC_API HashTableV2Item *
HashTableV2Insert(HashTableV2 *ht_ptr, HashTableV2Item *item_ptr,
                  bool *is_added_ptr)
{
	if (IS_EMPTY(ht_ptr) || IS_EMPTY(item_ptr)) return NULL;

	if (ht_ptr->enable_locking) {
		if (sWriteLock(ht_ptr, 0) != 0) return NULL;
	}

	/* Reject if at hard ceiling (max_entries set and reached) */
	if (sAtCapacity(ht_ptr)) {
		if (ht_ptr->enable_locking) sUnlock(ht_ptr);
		if (IS_PRESENT(is_added_ptr)) *is_added_ptr = false;
		return NULL;
	}

	/* Expand if at load-factor threshold */
	if (sNeedsExpand(ht_ptr))
		sExpandTable(ht_ptr);

	const void *key_ptr = sExtractKey(ht_ptr, item_ptr);
	HashTableV2Slot *slot;
	size_t idx = sFindSlot(ht_ptr, key_ptr, true /* insert_mode */, &slot);

	if (IS_EMPTY(slot)) {
		/* Table is completely full — cannot insert */
		if (ht_ptr->enable_locking) sUnlock(ht_ptr);
		if (IS_PRESENT(is_added_ptr)) *is_added_ptr = false;
		return NULL;
	}

	/* Check if this is a duplicate (occupied slot with matching key) */
	if (!sSlotIsClaimable(slot)) {
		/* Duplicate — the slot already holds an item with this key */
		if (ht_ptr->enable_locking) sUnlock(ht_ptr);
		if (IS_PRESENT(is_added_ptr)) *is_added_ptr = true;
		return *slot;   /* return the existing item */
	}

	/* Fresh insertion */
	*slot = item_ptr;
	ht_ptr->num_entries++;

	if (ht_ptr->enable_locking) sUnlock(ht_ptr);
	if (IS_PRESENT(is_added_ptr)) *is_added_ptr = false;

	return item_ptr;
}

static HashTableV2Item *
sLookup(HashTableV2 *ht_ptr, const void *key_ptr)
{
	HashTableV2Slot *slot;
	sFindSlot(ht_ptr, key_ptr, false /* lookup */, &slot);
	return IS_PRESENT(slot) ? *slot : NULL;
}

PUBLIC_API HashTableV2Item *
HashTableV2Lookup(HashTableV2 *ht_ptr, const void *key_ptr)
{
	if (IS_EMPTY(ht_ptr) || IS_EMPTY(key_ptr)) return NULL;

	if (ht_ptr->enable_locking) {
		if (sReadLock(ht_ptr, 0) != 0) return NULL;
	}

	HashTableV2Item *result = sLookup(ht_ptr, key_ptr);

	if (ht_ptr->enable_locking) sUnlock(ht_ptr);

	return result;
}

PUBLIC_API HashTableV2Item *
HashTableV2Remove(HashTableV2 *ht_ptr, HashTableV2Item *item_ptr)
{
	if (IS_EMPTY(ht_ptr) || IS_EMPTY(item_ptr)) return NULL;

	if (ht_ptr->enable_locking) {
		if (sWriteLock(ht_ptr, 0) != 0) return NULL;
	}

	const void *key_ptr = sExtractKey(ht_ptr, item_ptr);
	HashTableV2Slot *slot;
	sFindSlot(ht_ptr, key_ptr, false /* lookup */, &slot);

	if (IS_EMPTY(slot) || *slot == PRIV_HASHTABLE_V2_SLOT_TOMBSTONE) {
		/* Not found */
		if (ht_ptr->enable_locking) sUnlock(ht_ptr);
		return NULL;
	}

	/* Mark as TOMBSTONE — preserves probe chains */
	HashTableV2Item *removed = *slot;
	*slot = PRIV_HASHTABLE_V2_SLOT_TOMBSTONE;
	ht_ptr->num_entries--;

	if (ht_ptr->enable_locking) sUnlock(ht_ptr);

	return removed;
}

/* ═══════════════════════════════════════════════════════════════════
 * Query
 * ═══════════════════════════════════════════════════════════════════ */

PUBLIC_API size_t
HashTableV2Size(HashTableV2 *ht_ptr)
{
	if (IS_EMPTY(ht_ptr)) return 0;
	return ht_ptr->num_entries;
}

PUBLIC_API size_t
HashTableV2Capacity(HashTableV2 *ht_ptr)
{
	if (IS_EMPTY(ht_ptr)) return 0;
	return ht_ptr->capacity;
}

/* ═══════════════════════════════════════════════════════════════════
 * Bulk Operations
 * ═══════════════════════════════════════════════════════════════════ */

PUBLIC_API long
HashTableV2Enumerate(HashTableV2 *ht_ptr, HashTableV2Item **out_array_ptr,
                     long array_capacity)
{
	if (IS_EMPTY(ht_ptr) || IS_EMPTY(out_array_ptr) || array_capacity <= 0)
		return -1;

	if (ht_ptr->enable_locking) {
		if (sReadLock(ht_ptr, 0) != 0) return -1;
	}

	long count = 0;
	for (size_t i = 0; i < ht_ptr->capacity && count < array_capacity; i++) {
		HashTableV2Slot *slot = &ht_ptr->slots[i];
		if (!IS_EMPTY(*slot) && *slot != PRIV_HASHTABLE_V2_SLOT_TOMBSTONE) {
			out_array_ptr[count++] = *slot;
		}
	}

	if (ht_ptr->enable_locking) sUnlock(ht_ptr);

	return count;
}

PUBLIC_API void
HashTableV2Merge(HashTableV2 *dest_ptr, HashTableV2 *src_ptr)
{
	if (IS_EMPTY(dest_ptr) || IS_EMPTY(src_ptr)) return;

	if (dest_ptr->enable_locking) {
		if (sWriteLock(dest_ptr, 0) != 0) return;
	}

	for (size_t i = 0; i < src_ptr->capacity; i++) {
		HashTableV2Slot *slot = &src_ptr->slots[i];
		if (!IS_EMPTY(*slot) && *slot != PRIV_HASHTABLE_V2_SLOT_TOMBSTONE) {
			HashTableV2Insert(dest_ptr, *slot, NULL);
		}
	}

	if (dest_ptr->enable_locking) sUnlock(dest_ptr);
}

/* ═══════════════════════════════════════════════════════════════════
 * Internal Locking Helpers (static — called from Insert/Lookup/Remove)
 * ═══════════════════════════════════════════════════════════════════ */

static int
sReadLock(HashTableV2 *ht_ptr, int try_flag)
{
	if (IS_EMPTY(ht_ptr) || !ht_ptr->enable_locking) return 0;

	if (try_flag)
		return pthread_rwlock_tryrdlock(&ht_ptr->rwlock);

	return pthread_rwlock_rdlock(&ht_ptr->rwlock);
}

static int
sWriteLock(HashTableV2 *ht_ptr, int try_flag)
{
	if (IS_EMPTY(ht_ptr) || !ht_ptr->enable_locking) return 0;

	if (try_flag)
		return pthread_rwlock_trywrlock(&ht_ptr->rwlock);

	return pthread_rwlock_wrlock(&ht_ptr->rwlock);
}

static int
sUnlock(HashTableV2 *ht_ptr)
{
	if (IS_EMPTY(ht_ptr) || !ht_ptr->enable_locking) return 0;

	return pthread_rwlock_unlock(&ht_ptr->rwlock);
}

/* ═══════════════════════════════════════════════════════════════════
 * External Locking (public API — delegates to internal helpers)
 * ═══════════════════════════════════════════════════════════════════ */

PUBLIC_API int
HashTableV2ReadLock(HashTableV2 *ht_ptr, int try_flag)
{
	return sReadLock(ht_ptr, try_flag);
}

PUBLIC_API int
HashTableV2WriteLock(HashTableV2 *ht_ptr, int try_flag)
{
	return sWriteLock(ht_ptr, try_flag);
}

PUBLIC_API int
HashTableV2Unlock(HashTableV2 *ht_ptr)
{
	return sUnlock(ht_ptr);
}
