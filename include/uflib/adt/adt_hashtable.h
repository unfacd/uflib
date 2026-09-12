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

#ifndef UFLIB_ADT_ADT_HASHTABLE_H
#define UFLIB_ADT_ADT_HASHTABLE_H

#include <uflib/uflib_defs.h>
#include <stdbool.h>

#include <uflib/adt/adt_hashtable_type.h>
#include <uflib/recycler/recycler_type.h>

#define _CONFIGDEFAULT_HASHTABLE_EXPANSION_SZ							65521

#define KEY_SIZE_ZERO 0
#define HASH_ITEM_IS_PTR_TYPE 1
#define HASH_ITEM_NOT_PTR_TYPE 0
#define HASHTABLE_NAME(x)	(x->table_name)
#define HASHTABLE_MAXSIZE(x)	(x)->max_size
#define HASHTABLE_SIZE(x)	(x->fTableSize)
#define HASHTABLE_ENTRIES(x)	(x->fNumEntries)
#define HASHTABLE_ITEM_EXTRACTOR_CALLBACK(x)	(x->item_extractor_callback)
#define HASHTABLE_LOCK(x)	(x->hashtable_rwlock)
#define HASHTABLE_KEYSIZE(x)	(x->fKeySize)
#define HASHTABLE_KEYOFFSET(x)	(x->fKeyOffset)
#define HASTABLE_ISTABLELOCKING(x)	(x->flag_locking)
#define HASTABLE_ISTABLERESIZABLE(x)	((x)->flag_resizable==1)
#define HASHTABLE_ISKEYPTR(x)	(x->fKeyIsPtr)
#define HASHTABLE_DATA(x)	(x->fTable)

#define HASHTABLE_SETFLAG(x, y)	(x)->y=1
#define HASHTABLE_CLEARFLAG(x, y)	(x)->y=0

#define HASHTABLE_EXTRACT_ITEM(x, y) ((*x->item_extractor_callback)(y))
#define HASHTABLE_DEFAULT_EXTRACTOR NULL
#define HASHTABLE_DONT_INDICATE_IS_ADDED NULL

#define HASHTABLE_SELF_DESTRUCT true
#define HASHTABLE_ITEM_CONTAINER_OFFSET_ZERO 0

typedef void *(*evictor_callback)(ContextData *);

/**
 * @brief Duplicate detection semantics
 *
 * This hash table detects duplicates by comparing the **extracted item pointers**
 * (keys) — NOT the container pointers and NOT the key values themselves.
 *
 * Two different containers that hold the same key value (e.g. session_id = 42 at
 * different memory addresses) are inserted as separate entries.  The hash table
 * only prevents inserting the exact same container pointer twice.
 *
 * The caller is responsible for ensuring key-value uniqueness.  If you need
 * key-value uniqueness enforcement, verify with HashLookup() before AddToHash().
 */

HashTable *HashTableInstantiate(HashTable *, int offset, int size, long isPtr, const char *, ItemExtractor item_extractor_callback);
PUBLIC_API HashTable* HashTableLockingInstantiate(HashTable *ht_ptr_in, int offset, int size, long isPtr, const char *, ItemExtractor item_extractor_callback);
PUBLIC_API void HashTableDestruct(HashTable *hash, bool is_self_destruct);

PUBLIC_API void *AddToHash(HashTable* hash, void* item);
PUBLIC_API void *AddToHashWithIndication(HashTable *hash, void *item_container, bool *is_added);
PUBLIC_API void *AddToHashWithReference (HashTable *hash, void *item, void(*reference_incrementer_callback)(RecyclerClientData *, int));
PUBLIC_API void *AddToHashEvictIfNecessary (HashTable *hasht_ptr, const void *item_key, void *item_container_ptr, evictor_callback, ContextData *, void **);
PUBLIC_API void *AddToHashEvictIfNecessaryWithReference (HashTable *hasht_ptr,  const void *item_key, void *item_container_ptr, void *(*item_evictor_callback)(ContextData *), void **, void(*reference_incrementer_callback)(RecyclerClientData *, int), void(*reference_decrementer_callback)(RecyclerClientData *, int));
PUBLIC_API void *HashLookup(HashTable* hash, void* data, bool);
PUBLIC_API void *HashLookupWithReference(HashTable *hash, const void *item_key, void(*reference_incrementer_callback)(RecyclerClientData *, int));
PUBLIC_API void *RemoveFromHash(HashTable *hash, void *item_container);
PUBLIC_API void *RemoveFromHashWithReference(HashTable *hash, void *item_value_ptr, void(*reference_decrementor_callback)(RecyclerClientData *, int));

PUBLIC_API void MergeHashEntries(HashTable* destination, HashTable* source);
PUBLIC_API int GetHashEntries(HashTable* hash, void** itemArray, long itemArraySize);


PUBLIC_API int HashTable_RdLock (HashTable *ht_ptr, int);
PUBLIC_API int HashTable_WrLock (HashTable *ht_ptr, int);
PUBLIC_API int HashTable_UnLock (HashTable *ht_ptr);

#endif /* UFLIB_ADT_ADT_HASHTABLE_H */
