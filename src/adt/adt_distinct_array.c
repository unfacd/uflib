/*

 Copyright (c) 2015-2026 unfacd works

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
 * @file adt_distinct_array.c
 * @brief Duplicate-free variable-size array — slab-allocated, append-only,
 *        with an embedded HashTable for O(1) duplicate detection.
 *
 * Items are stored in contiguous fixed-width slots.  The array grows in
 * power-of-2 increments via @c realloc.  Removal is tail-end only and exists
 * for error-path rollback after a failed hash-table insert.
 *
 * Ownership: Put copies the supplied bytes into the slab; index and hash
 * store pointers into that slab. Pointers returned by RemoveLast must not
 * be freed by the caller.
 */

#include <uflib/adt/adt_distinct_array.h>
#include <stdio.h>
#include <string.h>
#include <sys/syslog.h>

static int sDistinctArrayPut(DistinctArray *hash_map_ptr, struct VariableBlockIndex **type_collection, uint8_t **block_storage, uint8_t *ufrvuid, size_t entry_sz);
static int sDistinctArrayRemoveLast(DistinctArray *hash_map_ptr);

/**
 * @brief Insert an item if it is not already present in the hash table.
 *
 * Duplicate check, slab insertion, hash-table registration, and rollback on
 * hash-table failure are all handled atomically from the caller's perspective.
 *
 * @param distinct_array_ptr  The DistinctArray to insert into.
 * @param type_collection     Pointer to the array's @c stored_value_idx member.
 * @param block_storage       Pointer to the array's @c value_block_storage member.
 * @param ufrvuid             Item data to store (copied into the slab).
 * @param entry_sz            Size of @p ufrvuid in bytes.
 * @return @c DistinctArrayResult_Ok, @c DistinctArrayResult_Duplicate, or
 *         @c DistinctArrayResult_HashError.
 *
 * FIX: allocation / oversized-entry failure now returns HashError (was
 *      incorrectly reported as Duplicate). AddToHash receives the
 *      slab-resident pointer.
 */
int
DistinctArrayPut(DistinctArray *distinct_array_ptr, struct VariableBlockIndex **type_collection, uint8_t **block_storage, uint8_t *ufrvuid, size_t entry_sz)
{
  if (IS_EMPTY(HashLookup(&distinct_array_ptr->hashTable, (void *)ufrvuid, true))) {
    if (sDistinctArrayPut(distinct_array_ptr, type_collection, block_storage, ufrvuid, entry_sz) == 0) {
      /* FIX: use the pointer that now lives in the slab */
      uint8_t *stored = type_collection[0]->value_index[type_collection[0]->size - 1];
      if (IS_PRESENT(AddToHash(&distinct_array_ptr->hashTable, stored))) {
        // diagnostic only — callback is optional
        if (distinct_array_ptr->hashTable.item_pretty_printer_callback) {
          distinct_array_ptr->hashTable.item_pretty_printer_callback(stored);
        }
        return DistinctArrayResult_Ok;
      } else {
        syslog(LOG_DEBUG, "%s: ERROR: ENTRY NOT HashMapped — rolling back array insertion", __func__);
        sDistinctArrayRemoveLast(distinct_array_ptr);
        return DistinctArrayResult_HashError;
      }
    } else {
      /* FIX: sPut failure (OOM or entry_sz > unit) was falling through to Duplicate */
      return DistinctArrayResult_HashError;
    }
  }

  return DistinctArrayResult_Duplicate;
}

/**
 * @brief Append an item to the slab storage, growing the allocation if needed.
 *
 * Growth uses a power-of-2 strategy: @c realloc is called only when
 * @c (current_sz & incremented_sz) == 0 — i.e. at sizes 0→1, 1→2, 3→4,
 * 7→8, etc.  The @c value_index and block storage are kept parallel.
 *
 * @param hash_map_ptr    The DistinctArray.
 * @param type_collection  Pointer to the @c stored_value_idx member.
 * @param block_storage    Pointer to the @c value_block_storage member.
 * @param ufrvuid          Item data to copy into the slab.
 * @param entry_sz         Size of @p ufrvuid in bytes.
 * @return 0 on success, 1 on allocation failure.
 *
 * FIX: grow index first then block (avoids leak/dangling on partial failure).
 * FIX: reject entry_sz larger than block_storage_unit_sz.
 * FIX: value_index stores the in-slab address (ownership of the copy).
 */
/* FIX: realloc() may relocate the slab.  value_index[] and the HashTable both
 * store raw pointers into value_block_storage, so a move leaves them dangling
 * (heap-use-after-free on the next lookup / iteration).  Re-base every already
 * stored entry and re-bucket the hash table from scratch, preserving the hash
 * table's configured key offset / size / pointer-ness / extractor and name. */
static void
sDistinctArrayRebase(DistinctArray *hash_map_ptr, size_t count)
{
  DistinctArrayDescriptor *desc_ptr = &hash_map_ptr->distinct_array_descriptor;
  uint8_t *new_block = (uint8_t *)hash_map_ptr->value_block_storage;

  for (size_t i = 0; i < count; i++) {
    hash_map_ptr->stored_value_idx->value_index[i] =
        new_block + desc_ptr->storage_slot_offset + (desc_ptr->block_storage_unit_sz * i);
  }

  long          key_offset = hash_map_ptr->hashTable.fKeyOffset;
  long          key_size   = hash_map_ptr->hashTable.fKeySize;
  long          key_is_ptr = hash_map_ptr->hashTable.fKeyIsPtr;
  ItemExtractor extractor  = hash_map_ptr->hashTable.item_extractor_callback;
  char          saved_name[128];
  snprintf(saved_name, sizeof(saved_name), "%s",
           hash_map_ptr->hashTable.table_name ? hash_map_ptr->hashTable.table_name : "DistinctArray");

  /* Re-instantiating frees the previous fTable and table_name, so the name and
   * key parameters above were captured first. */
  HashTableInstantiate(&hash_map_ptr->hashTable, (int)key_offset, (int)key_size,
                       key_is_ptr, saved_name, extractor);

  for (size_t i = 0; i < count; i++) {
    AddToHash(&hash_map_ptr->hashTable, hash_map_ptr->stored_value_idx->value_index[i]);
  }
}

static int
sDistinctArrayPut(DistinctArray *hash_map_ptr, struct VariableBlockIndex **type_collection, uint8_t **block_storage, uint8_t *ufrvuid, size_t entry_sz)
{
  DistinctArrayDescriptor *desc_ptr = &hash_map_ptr->distinct_array_descriptor;
  size_t current_sz = *type_collection ? type_collection[0]->size : 0;
  size_t incremented_sz = current_sz + 1;

  /* FIX: guard against overflow of the fixed-width slot */
  if (entry_sz > desc_ptr->block_storage_unit_sz) {
    return 1;
  }

  if ((current_sz & incremented_sz) == 0) {
    size_t new_idx_bytes = sizeof **type_collection + ((current_sz + incremented_sz) * sizeof type_collection[0]->value_index[0]);
    size_t new_block_bytes = desc_ptr->block_storage_unit_sz + ((current_sz + incremented_sz) * desc_ptr->block_storage_unit_sz);

    /* FIX: allocate / grow index first and commit before touching block storage */
    void *variable_array_allocated = realloc(*type_collection, new_idx_bytes);
    if (IS_EMPTY(variable_array_allocated)) {
      return 1;
    }
    *type_collection = variable_array_allocated;

    uint8_t *old_block = *block_storage;
    void *variable_storage_allocated = realloc(*block_storage, new_block_bytes);
    if (IS_EMPTY(variable_storage_allocated)) {
      /* index is larger than needed but size not updated — safe, no leak */
      return 1;
    }

    desc_ptr->collection_allocated_sz = new_idx_bytes;
    desc_ptr->block_allocated_sz = new_block_bytes;

    syslog(LOG_DEBUG, "--Allocating (current_sz:'%lu', incremented_sz:'%lu'): %lu bytes\n", current_sz, incremented_sz, desc_ptr->collection_allocated_sz);
    syslog(LOG_DEBUG, "--Allocating block size: %lu bytes, total slots available: '%lu'.\n", desc_ptr->block_allocated_sz, desc_ptr->block_allocated_sz / desc_ptr->block_storage_unit_sz);
    *block_storage = variable_storage_allocated;

    /* FIX: realloc may have relocated the slab — re-base the stored pointers.
     * current_sz (== the old size) is the count of already-stored entries; pass
     * it explicitly because the freshly realloc'd index array's ->size is still
     * uninitialised on the very first insert. */
    if ((uint8_t *)variable_storage_allocated != old_block && current_sz > 0) {
      sDistinctArrayRebase(hash_map_ptr, current_sz);
    }
  }

  /* FIX: compute in-slab address, copy, and store that address (not the original) */
  uint8_t *slot = (uint8_t *)(*block_storage + desc_ptr->storage_slot_offset) + (desc_ptr->block_storage_unit_sz * current_sz);
  memcpy(slot, ufrvuid, entry_sz);
  type_collection[0]->value_index[current_sz] = slot;
  type_collection[0]->size = incremented_sz;
  syslog(LOG_DEBUG, "--Total slots used: '%lu'. Current slots available: '%lu', " "Total slots available: '%lu'. Address idx():'%p'\n", current_sz, (desc_ptr->block_allocated_sz / desc_ptr->block_storage_unit_sz) - current_sz, desc_ptr->block_allocated_sz / desc_ptr->block_storage_unit_sz, slot);

  return 0;
}

/**
 * @brief Remove the last-inserted item from the array (tail-end removal).
 *
 * Nulls the value-index entry and decrements the array size.  The block-storage
 * slot's data is deliberately left in place: the public RemoveLast() returns
 * that in-slab pointer and the caller must be able to read it.  The slot is
 * overwritten by the next Put() into this index.  Does not shrink the allocation.
 * Also used as a rollback mechanism when hash-table insertion fails after a
 * successful slab append.
 *
 * @param hash_map_ptr  The DistinctArray to remove the last item from.
 * @return 0 on success, 1 if the array is empty or uninitialised.
 */
static int
sDistinctArrayRemoveLast(DistinctArray *hash_map_ptr)
{
  if (!hash_map_ptr->stored_value_idx || hash_map_ptr->stored_value_idx->size == 0) {
    return 1;
  }

  size_t last_idx = hash_map_ptr->stored_value_idx->size - 1;

  hash_map_ptr->stored_value_idx->value_index[last_idx] = NULL;
  hash_map_ptr->stored_value_idx->size = last_idx;

  return 0;
}

/**
 * @brief Remove the last-inserted item from the array and the hash table.
 *
 * This is the public, hash-table-aware complement of the internal rollback
 * function.  It removes the item from both the embedded HashTable and the
 * slab storage, then returns the removed item pointer.
 *
 * @param hash_map_ptr  The DistinctArray to remove the last item from.
 * @return Pointer to the removed item (in-slab; do not free), or @c NULL
 *         if the array is empty or uninitialised.
 *
 * @note Only tail-end removal is supported.  This is a design choice:
 *       the primary use case is as a static hashmap where removal is rare
 *       (configuration reloads, error-path rollback).  Arbitrary-position
 *       removal would require O(n) compaction or swap-with-last (which
 *       changes iteration order).  These are noted as future roadmap items.
 *
 * FIX: returned pointer is into the slab; caller must not free it.
 *
 * @code{.c}
 * // Remove items until empty, then re-fill
 * while ((item = DistinctArrayRemoveLast(&da))) {
 *     // inspect if needed; do NOT free
 * }
 * // re-fill — allocation is preserved, no realloc needed
 * DistinctArrayPut(&da, &da.stored_value_idx,
 *                  (uint8_t **)&da.value_block_storage,
 *                  (uint8_t *)"new_item", 10);
 * @endcode
 */
void *
DistinctArrayRemoveLast(DistinctArray *hash_map_ptr)
{
  if (!hash_map_ptr->stored_value_idx || hash_map_ptr->stored_value_idx->size == 0) {
    return NULL;
  }

  size_t last_idx = hash_map_ptr->stored_value_idx->size - 1;

  // Capture the item pointer before the slab removal nulls it
  uint8_t *removed_item = hash_map_ptr->stored_value_idx->value_index[last_idx];

  if (IS_PRESENT(removed_item)) {
    // Remove from the hash table so the duplicate-detection index stays in sync
    RemoveFromHash(&hash_map_ptr->hashTable, removed_item);
  }

  sDistinctArrayRemoveLast(hash_map_ptr);

  return removed_item;
}

/**
 * @brief Iterate over every populated slot, invoking @p on_item_available
 *        for each non-NULL entry.
 *
 * @param hash_map_ptr       The DistinctArray to iterate.
 * @param on_item_available  Block called with a pointer to each stored item.
 *
 * @note Requires Clang (@c -fblocks) — the callback is an Objective-C block.
 *
 * FIX: walks stored_value_idx->value_index[] (previous value_block_storage[i]
 *      access was undefined behaviour).
 */
void
DistinctArrayIterate(DistinctArray *hash_map_ptr,
                     void(^on_item_available)(uint8_t *item))
{
  if (!hash_map_ptr->stored_value_idx || !hash_map_ptr->value_block_storage) {
    return;
  }

  for (size_t i = 0; i < hash_map_ptr->stored_value_idx->size; i++) {
    uint8_t *item = hash_map_ptr->stored_value_idx->value_index[i];
    if (IS_PRESENT(item)) {
      on_item_available(item);
    }
  }
}

/**
 * @brief Look up an item key in the embedded hash table.
 *
 * @param hash_map_ptr  The DistinctArray to search.
 * @param item          Item key to look up.
 * @return Non-NULL if the item is present, NULL otherwise.
 */
void *
DistinctArrayLookupItem(DistinctArray *hash_map_ptr, uint8_t *item)
{
  return HashLookup(&hash_map_ptr->hashTable, item, true);
}

/**
 * @brief Test whether an item is stored in the array.
 *
 * Convenience wrapper around DistinctArrayLookupItem().
 *
 * @param hash_map_ptr  The DistinctArray to search.
 * @param item          Item key to test.
 * @return @c true if the item is present, @c false otherwise.
 */
bool
DistinctArrayIsItemStored(DistinctArray *hash_map_ptr, uint8_t *item)
{
  return IS_PRESENT(DistinctArrayLookupItem(hash_map_ptr, item));
}

/**
 * @brief Free all storage owned by the array.
 *
 * Destroys the embedded HashTable (without freeing its internal table —
 * the caller owns that memory) and frees the slab-allocated block and
 * index arrays.
 *
 * @param hash_map_ptr  The DistinctArray to destroy.
 */
void
DistinctArrayDestruct(DistinctArray *hash_map_ptr)
{
  HashTableDestruct(&hash_map_ptr->hashTable, !HASHTABLE_SELF_DESTRUCT);
  free(hash_map_ptr->value_block_storage);
  free(hash_map_ptr->stored_value_idx);
}
