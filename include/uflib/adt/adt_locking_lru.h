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

#ifndef UFLIB_ADT_ADT_LOCKING_LRU_H
#define UFLIB_ADT_ADT_LOCKING_LRU_H

#include <uflib/uflib_defs.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness" //clang went berserk with warnings

#include <uflib/adt/adt_hashtable.h>
#include <uflib/recycler/instance_type.h>
#include <uflib/adt/adt_locking_lru_type.h>

typedef void (*hashtable_init_callback)(HashTable *, size_t, unsigned long);
typedef void InstanceHolderForLruItem;

typedef bool (*lru_item_matcher)(LruClientData *client_data_ptr_stored, LruClientData *client_data_ptr_provided); ///> user provided callback for matching a stored lru item
#define LOCKINGLRU_MATCH_ITEM(x, y, z) ((x)((LruClientData *)y, (LruClientData *)z))

#define LOCKINGLRU_EXTRACT_ITEM(x, y) ((*x->item_extractor_callback)(y))
#define LOCKINGLRU_PRINT_ITEM(x, y, z) ((*x->item_printer_callback)(y, z))

LockingLru *
InitLockingLru(LockingLru *lru_ptr_in, const char *lru_name, size_t, size_t allocation_sz, HashTable *hash_table_ptr,
               hashtable_init_callback, ItemExtractor, ItemPrinter);
void *LockingLruSet(LockingLru *lru_ptr, LruClientData *client_data_ptr);
void *LockingLruGet(LockingLru *lru_ptr, const void *, LruClientData **client_data_ptr_evicted);
LockingLru *LockingLruAddToFront(LockingLru *lru_ptr, LruClientData *data, LockingLruItem *lru_item_in, bool flag_lock);
LockingLru *LockingLruAddToRear(LockingLru *lru_ptr, LruClientData *data, LockingLruItem *lru_item_in, bool flag_lock);
void *LockingLruRemoveFromFront(LockingLru *lru_ptr);
void *LockingLruRemoveFromRear(LockingLru *lru_ptr);
void *LockingLruRemoveThis(LockingLru *lru_ptr, LruClientData *client_data_ptr);
PUBLIC_API size_t LockingLruSize(LockingLru *lru_ptr);
PUBLIC_API size_t LockingLruDescribeItems(LockingLru *lru_ptr);
void *LockingLruPromote(LockingLru *lru_ptr, const void *item_key, LruClientData *client_data_ptr);
bool LockingLruRearItemCheck(LockingLru *lru_ptr, LruClientData *client_data_ptr, ContextData *, void(*lrucache_headitemcheck_callback) (LruClientData *, bool));
LruClientData *LockingLruRearItemGet(LockingLru *lru_ptr);
LruClientData *LockingLruHeadItemGet(LockingLru *lru_ptr);
PUBLIC_API LruClientData * FindLruItemWithMatcherAndDelink(LockingLru *lru_ptr, lru_item_matcher matcher_callback, LruClientData *client_data_ptr_provided);
LockingLruItem *
FindLruItemWithMatcher(LockingLru *lru_ptr, lru_item_matcher matcher, LruClientData *client_data_ptr_provided);

PUBLIC_API void InitLockingLruItemRecyclerTypePool(size_t group_allocation_sz);
PUBLIC_API void LockingLruItemIncrementReference(InstanceHolderForLruItem *, int multiples);
PUBLIC_API void LockingLruItemDecrementReference(InstanceHolderForLruItem *, int multiples);
PUBLIC_API unsigned LockingLruItemPoolTypeNumber();
InstanceHolder *LockingLruItemGetInstance(ContextData *ctx_data_ptr, unsigned long call_flags);
PUBLIC_API void LockingLruItemReturnToRecycler(InstanceHolder *, ContextData *ctx_data_ptr, unsigned long call_flags);

PUBLIC_API void LockingLruLock(LockingLru *lru_ptr, void **lock_hook);
PUBLIC_API void LockingLruUnLock(LockingLru *lru_ptr, void *lock_hook);

#pragma clang diagnostic pop

#endif /* UFLIB_ADT_ADT_LOCKING_LRU_H */
