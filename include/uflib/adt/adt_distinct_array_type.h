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

#ifndef UFLIB_ADT_ADT_DISTINCT_ARRAY_TYPE_H
#define UFLIB_ADT_ADT_DISTINCT_ARRAY_TYPE_H

#include <stddef.h>
#include <stdint.h>
#include <uflib/adt/adt_hashtable.h>

/*! Descriptor tracking the allocation state of a DistinctArray's slab storage. */
typedef struct DistinctArrayDescriptor {
    size_t block_allocated_sz;      ///< Total block storage allocated (multiple of @c block_storage_unit_sz)
    size_t collection_allocated_sz; ///< Total index-collection storage allocated (bytes)
    size_t block_storage_unit_sz;   ///< Fixed size of each stored item (e.g. @c sizeof("xxx.xxx.xxx.xxx") for IPv4)
    size_t storage_slot_offset;     ///< Byte offset from VariableBlock base to the @c value[] FAM
    __unused size_t storage_slot_idx; ///< (unused) Legacy slot tracker — retained for ABI compatibility
} DistinctArrayDescriptor;

/*! Callback invoked by DistinctArrayIterate for each populated slot.
 *
 * @p ctx_ptr is the opaque value handed to DistinctArrayIterate, passed through unmodified.
 */
typedef void (*DistinctArrayIterateCallback)(void *ctx_ptr, uint8_t *item);

/*! Duplicate-free variable-size array backed by contiguous slab allocation.
 *
 * Items are stored in fixed-width slots within a contiguous block.  An embedded
 * HashTable guarantees that no duplicate item is ever inserted.  The array
 * grows in power-of-2 increments and supports append-only operation (tail-end
 * removal exists for error-path rollback only).
 *
 * @note Consumers embed this struct by value — do not change the field layout
 *       without a MAJOR version bump (ABI break).
 *
 * Note: value_block_storage is typed as VariableBlock* for historical
 * reasons; the implementation treats the allocation as a flat byte buffer
 * of unit-sized slots. Do not access its struct members directly.
 */
typedef struct DistinctArray {
    HashTable                hashTable;                 ///< Embedded hash table for duplicate detection
    DistinctArrayDescriptor  distinct_array_descriptor; ///< Slab-allocation metadata
    struct VariableBlock {
        size_t    size;      ///< Number of populated value slots
        uint8_t  *value[];   ///< Flexible array member — one pointer per stored item
    } *value_block_storage;                             ///< Contiguous block holding item data
    struct VariableBlockIndex {
        size_t    size;         ///< Number of populated index entries
        uint8_t  *value_index[];///< Flexible array member — head pointer for each stored item
    } *stored_value_idx;                                 ///< Index array parallel to @c value_block_storage
} DistinctArray;

#endif /* UFLIB_ADT_ADT_DISTINCT_ARRAY_TYPE_H */