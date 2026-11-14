/**
 * Copyright (C) 2015-2021 unfacd works
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

#ifndef UFLIB_ADT_ADT_HOPSCOTCH_HASHTABLE_TYPE_H
#define UFLIB_ADT_ADT_HOPSCOTCH_HASHTABLE_TYPE_H

#include <stdlib.h>

//based on http://codecapsule.com/2013/08/11/hopscotch-hashing/
struct hopscotch_bucket {
  void *data;
  uint32_t hopinfo;
} __attribute__ ((aligned (8)));

struct hopscotch_bucket_keyed {
  void *data;
  uintptr_t key;
  uint32_t hopinfo;
} __attribute__ ((aligned (8)));

struct HopscotchHashtable {
  size_t pfactor;//changes per resize
  void *buckets;//changes per resize
  size_t count; //items stored; maintained by the table, not by the caller
};
typedef struct HopscotchHashtable HopscotchHashtable;

typedef struct  HopscotchHashtableConfigurable {
  HopscotchHashtable hashtable;
  size_t keylen;      ///> key length in bytes; 0 keys on the item pointer itself
  size_t key_offset;  ///> offset of the key inside the item when keylen is non-zero
  uint64_t (*hash_func)(const uint8_t *key_bytes, size_t key_len); ///> hashes the key the store hands it
  void * (*buckets_allocator_func)(size_t, size_t); //unit size * multiples (a la cmalloc)
} HopscotchHashtableConfigurable;

#endif //UFSRV_ADT_HOPSCOTCH_HASHTABLE_TYPE_H
