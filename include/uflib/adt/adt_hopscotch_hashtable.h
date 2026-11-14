/**
 * Copyright (C) 2015-2024 unfacd works
 *  Numerous interface mods.
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

#ifndef UFLIB_ADT_ADT_HOPSCOTCH_HASHTABLE_H
#define UFLIB_ADT_ADT_HOPSCOTCH_HASHTABLE_H

#include <uflib/uflib_defs.h>


/*_
 * Copyright (c) 2016 Hirochika Asai <asai@jar.jp>
 * With modifications Copyright (C) 2015-2021 unfacd works
 *
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


//#define HOPSCOTCH_INIT_BSIZE_FACTOR     6 //64 buckets
#define HOPSCOTCH_HOPINFO_SIZE          32
#define ITEM_EXTRACTOR_UNDEFINED NULL

#include <uflib/standard_c_includes.h>
#include <uflib/main_types.h>
#include <uflib/adt/adt_hopscotch_hashtable_type.h>

typedef ClientContextData * (*ItemExtractor)(ItemContainer *);

typedef struct UFSRVResult UFSRVResult;//todo should aliased to something else

typedef UFSRVResult *(*CallbackExecutor)(ClientContextData *, ClientContextData *payload);

typedef void(*CallbackFinaliser)(ClientContextData *);

PUBLIC_API struct HopscotchHashtable *hopscotch_init_with_offset(HopscotchHashtable *ht, size_t pfactor);
PUBLIC_API void hopscotch_release(HopscotchHashtable *);
PUBLIC_API void *hopscotch_lookup_with_offset(HopscotchHashtable *ht, ItemExtractor extractor_ptr, uint8_t *key, size_t key_offset);
/*
 *  A HopscotchHashtableConfigurable does not decide how its keys are formed; the
 *  consumer does, through three fields it must set before the first insert:
 *
 *      htc->keylen       key length IN BYTES, or 0 for pointer identity
 *      htc->key_offset   where inside the item the key bytes start (content mode)
 *      htc->hash_func    hashes exactly the bytes this store hands it
 *
 *  ----------------------------------------------------------------------------
 *  keylen > 0   CONTENT KEY   — the key is the bytes that live inside the item
 *  ----------------------------------------------------------------------------
 *      key material : keylen bytes starting at (uint8_t *)item + key_offset
 *      hash         : hash_func(item + key_offset, keylen)
 *      equality     : memcmp(key, stored, keylen) == 0
 *
 *      Requirement: those keylen bytes must be IMMUTABLE for as long as the item
 *      resides in the table.
 *
 *      This is the correct mode for records whose leading field is a stable
 *      identifier (an id, a hash, a fixed name) and the wrong mode for objects
 *      whose leading fields are mutable state.  If the "key" you have in mind
 *      is the object's identity, use identity mode.
 *
 *  ----------------------------------------------------------------------------
 *  keylen == 0  IDENTITY KEY  — the item pointer IS the key
 *  ----------------------------------------------------------------------------
 *      key material : the bytes of the item pointer itself (sizeof(void *))
 *      hash         : hash_func(&item, sizeof(item))
 *      equality     : key == stored   (pointer comparison)
 *
 *      Use this for object sets ("which objects does this thread hold", "which
 *      entities are registered") where membership is a property of the object's
 *      identity, not of its contents.  Nothing inside the object is read, so no
 *      amount of mutation can invalidate the key.  key_offset is ignored.
 *
 *  ----------------------------------------------------------------------------
 *  Choosing hash_func
 *  ----------------------------------------------------------------------------
 *      The store calls  hash_func(bytes, length)  in BOTH modes, and a correct
 *      hash must consume exactly `length` bytes:
 *
 *        content mode  hash_bytes_u64(item + key_offset, keylen)   — any length
 *        identity mode hash_bytes_u64(&item, sizeof(item))         — 8 bytes
 *
 *      hash_bytes_u64() (FNV-1a over all bytes, avalanche-finished) is the right
 *      choice in both modes.
 *
 *      WARNING: do not install a scalar hash that ignores its length argument,
 *      such as inthash_u64(uint64_t key, size_t key_len).  Its signature makes
 *      the first parameter a value, while this store passes a POINTER TO the
 *      bytes; through this function-pointer type it would hash the address of
 *      the caller's local variable, giving a different key on every call.
 *
 *  ----------------------------------------------------------------------------
 *  Worked examples
 *  ----------------------------------------------------------------------------
 *      Content key on a stable identifier at the head of the record:
 *
 *          htc->keylen     = sizeof(((MyRecord *)0)->id);
 *          htc->key_offset = offsetof(MyRecord, id);
 *          htc->hash_func  = hash_bytes_u64;
 *
 *      Identity key on the object pointer:
 *
 *          htc->keylen     = 0;
 *          htc->key_offset = 0;               // ignored
 *          htc->hash_func  = hash_bytes_u64;  // hashes the pointer's bytes
 *
 *  The offset-based API above (hopscotch_*_with_offset) is a different contract:
 *  it hashes with a fixed internal hash over an externally supplied key, and it
 *  is not affected by keylen.  The two APIs must not be mixed on one table.
 */
PUBLIC_API void *hopscotch_lookup_configurable(HopscotchHashtableConfigurable *htc, uint8_t *key);
PUBLIC_API int hopscotch_insert_with_offset(HopscotchHashtable *ht, ItemExtractor extractor_ptr, void *data, size_t key_offset);
PUBLIC_API int hopscotch_insert_configurable(HopscotchHashtableConfigurable *htc, uint8_t *data);
PUBLIC_API void *hopscotch_remove_with_offset(HopscotchHashtable *ht, ItemExtractor extractor_ptr, uint8_t *key, size_t key_offset);
PUBLIC_API void *hopscotch_remove_configurable(HopscotchHashtableConfigurable *htc, uint8_t *key);

PUBLIC_API struct HopscotchHashtable *hopscotch_init(HopscotchHashtable *ht, size_t pfactor);
PUBLIC_API void *hopscotch_lookup(HopscotchHashtable *ht, intptr_t key);
PUBLIC_API int hopscotch_insert(HopscotchHashtable *ht, uintptr_t key, void *data);
PUBLIC_API void *hopscotch_remove(HopscotchHashtable *ht, uintptr_t key);
PUBLIC_API void *hopscotch_iterator_executor(HopscotchHashtable *ht, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr);

PUBLIC_API void *hopscotch_iterator_executor_configurable(HopscotchHashtableConfigurable *htc, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr);
PUBLIC_API size_t hopscotch_iterator_executor_with_offset(HopscotchHashtable *ht, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr);
PUBLIC_API void *hopscotch_iterator_finaliser(HopscotchHashtable *ht, CallbackFinaliser executor_ptr);

/*
 * WARNING: Released-table reuse hazard.
 *
 * A zero-initialized HopscotchHashtable (buckets==NULL, pfactor==0) is
 * indistinguishable from a table after hopscotch_release().  Calling insert on
 * a released table will compute hash & ((1<<0)-1) == 0 and dereference
 * buckets[0] (NULL) -> segfault with no diagnostic.
 *
 * Consumers must set their HopscotchHashtable to a known-invalid state after
 * release, or use the V2 opaque handle (HopscotchHashTable *) which eliminates
 * the zero-init path entirely.
 */
static inline bool IsHopscotchHashtableAllocated(HopscotchHashtable *ht_ptr) {
	return (IS_PRESENT(ht_ptr->buckets));
}

static inline size_t GetHopscotchHashtableAllocatedSize(HopscotchHashtable *ht_ptr) {
	return 1UL << ht_ptr->pfactor;
}

#endif /* UFLIB_ADT_ADT_HOPSCOTCH_HASHTABLE_H */
