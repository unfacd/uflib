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
#ifndef UFLIB_ADT_ADT_DISTINCT_ARRAY_H
#define UFLIB_ADT_ADT_DISTINCT_ARRAY_H

#include <uflib/uflib_defs.h>

/**
 * @file adt_distinct_array.h
 * @brief Duplicate-free variable-size array backed by contiguous slab
 *        allocation with an embedded HashTable for O(1) duplicate detection.
 *
 * Supports append (Put), lookup, iteration, and teardown.  Removal is
 * tail-end only and exists for error-path rollback.  The primary use
 * case is as a static hashmap (e.g. CORS allowed-origins registry).
 *
 * Ownership: Put copies the supplied bytes into the slab; the index and
 * hash store pointers into that slab. Pointers returned by RemoveLast
 * must not be freed by the caller.
 */

#include <stddef.h>
#include <stdint.h>
#include <uflib/adt/adt_distinct_array_type.h>

/*! Return codes for DistinctArrayPut(). */
typedef enum {
    DistinctArrayResult_Ok            = 0, ///< Item was inserted successfully
    DistinctArrayResult_Duplicate     = 1, ///< Item already present in the hash table
    DistinctArrayResult_HashError     = 2, ///< HashTable insert failed (item rolled back from array) or allocation / size error
} DistinctArrayResult;

/**
 * @brief Insert an item into the distinct array if it is not already present.
 *
 * The item is first added to the contiguous slab storage, then registered in
 * the embedded HashTable for duplicate detection.  If the hash-table insert
 * fails, the slab insertion is rolled back (tail-end removal).
 *
 * @param distinct_array_ptr  The DistinctArray to insert into.
 * @param type_collection     Pointer to the array's @c stored_value_idx member.
 * @param block_storage       Pointer to the array's @c value_block_storage member.
 * @param ufrvuid             Pointer to the item data to store.
 * @param entry_sz            Size of the item in bytes.
 *
 * @return @c DistinctArrayResult_Ok on success,
 *         @c DistinctArrayResult_Duplicate if the item already exists,
 *         @c DistinctArrayResult_HashError on hash-table insertion failure
 *         (also returned for allocation failure or entry_sz overflow).
 *
 * FIX: HashError is now returned for allocation failure and entry_sz
 *      overflow; previously those paths incorrectly reported Duplicate.
 *
 * @code{.c}
 * DistinctArray da = {0};
 * da.distinct_array_descriptor.block_storage_unit_sz = MAX_DOMAIN_SZ;
 * da.distinct_array_descriptor.storage_slot_offset =
 *     offsetof(struct VariableBlock, value);
 *
 * int rc = DistinctArrayPut(&da, &da.stored_value_idx,
 *                           (uint8_t **)&da.value_block_storage,
 *                           (uint8_t *)"example.com",
 *                           strlen("example.com") + 1);
 * if (rc != DistinctArrayResult_Ok) {
 *     // handle duplicate or error
 * }
 * DistinctArrayDestruct(&da);
 * @endcode
 */
PUBLIC_API int
DistinctArrayPut(DistinctArray *distinct_array_ptr,
                 struct VariableBlockIndex **type_collection,
                 uint8_t **block_storage,
                 uint8_t *ufrvuid,
                 size_t entry_sz);

/**
 * @brief Remove the last-inserted item from the array and the hash table.
 *
 * Returns the removed item pointer (in-slab; do not free).  Returns @c NULL
 * if the array is empty or uninitialised.
 *
 * @param hash_map_ptr  The DistinctArray to remove the last item from.
 * @return Pointer to the removed item, or @c NULL if the array is empty.
 *
 * @note Only tail-end removal is supported — a design choice for the
 *       static-hashmap use case.  See the implementation notes for future
 *       roadmap options (arbitrary-position removal with compaction or
 *       swap-with-last).
 *
 * FIX: returned pointer is into the slab; caller must not free it.
 *
 * @code{.c}
 * // Drain and re-fill
 * void *item;
 * while ((item = DistinctArrayRemoveLast(&da))) {
 *     // inspect if needed; do NOT free
 * }
 * // Slab allocation is preserved — re-fill without realloc
 * DistinctArrayPut(&da, &da.stored_value_idx,
 *                  (uint8_t **)&da.value_block_storage,
 *                  (uint8_t *)"fresh", 6);
 * @endcode
 */
PUBLIC_API void *
DistinctArrayRemoveLast(DistinctArray *hash_map_ptr);

/**
 * @brief Free all storage owned by the distinct array.
 *
 * Destroys the embedded HashTable (without freeing its internal table — the
 * caller owns that memory via @c value_block_storage and @c stored_value_idx)
 * and frees the slab-allocated block and index arrays.
 *
 * @param hash_map_ptr  The DistinctArray to destroy.
 *
 * @code{.c}
 * DistinctArrayDestruct(&da);
 * @endcode
 */
PUBLIC_API void
DistinctArrayDestruct(DistinctArray *hash_map_ptr);

/**
 * @brief Iterate over every populated slot in the array.
 *
 * The callback receives a pointer to each stored item.  Empty slots (NULL
 * entries) are skipped.
 *
 * @param hash_map_ptr   The DistinctArray to iterate.
 * @param callback_ptr   Invoked for each populated item; @p ctx_ptr is passed through unmodified.
 * @param ctx_ptr        Caller-owned context, opaque to the iteration.
 *
 * FIX: implementation now walks the value_index array (previous layout
 *      access was undefined behaviour).
 *
 * @code{.c}
 * static void sPrintItem(void *ctx_ptr, uint8_t *item) {
 *     printf("origin: %s\n", (char *)item);
 * }
 * DistinctArrayIterate(&da, sPrintItem, NULL);
 * @endcode
 */
PUBLIC_API void
DistinctArrayIterate(DistinctArray *hash_map_ptr, DistinctArrayIterateCallback callback_ptr, void *ctx_ptr);

/**
 * @brief Look up an item in the hash table.
 *
 * @param hash_map_ptr  The DistinctArray to search.
 * @param item          Pointer to the item key to look up.
 * @return Non-NULL if the item is present, NULL otherwise.
 *
 * @code{.c}
 * if (IS_PRESENT(DistinctArrayLookupItem(&da, (uint8_t *)key))) {
 *     // key exists
 * }
 * @endcode
 */
PUBLIC_API void *
DistinctArrayLookupItem(DistinctArray *hash_map_ptr, uint8_t *item);

/**
 * @brief Test whether an item is stored in the array.
 *
 * Convenience wrapper around DistinctArrayLookupItem().
 *
 * @param hash_map_ptr  The DistinctArray to search.
 * @param item          Pointer to the item key to test.
 * @return @c true if the item is present, @c false otherwise.
 *
 * @code{.c}
 * if (DistinctArrayIsItemStored(&da, (uint8_t *)origin)) {
 *     return true; // origin is allowed
 * }
 * @endcode
 */
PUBLIC_API bool
DistinctArrayIsItemStored(DistinctArray *hash_map_ptr, uint8_t *item);

#endif /* UFLIB_ADT_ADT_DISTINCT_ARRAY_H */