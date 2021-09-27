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

#ifndef UFSRV_ADT_HOPSCOTCH_HASHTABLE_TYPE_H
#define UFSRV_ADT_HOPSCOTCH_HASHTABLE_TYPE_H

#include <stdlib.h>

//based on http://codecapsule.com/2013/08/11/hopscotch-hashing/
struct hopscotch_bucket {
  void *data;
  uint32_t hopinfo;
};
struct HopscotchHashtable {
  size_t pfactor;//changes per resize
  struct hopscotch_bucket *buckets;//changes per resize
};
typedef struct HopscotchHashtable HopscotchHashtable;

typedef struct  HopscotchHashtableConfigurable {
  HopscotchHashtable hashtable;
  size_t keylen;
  size_t key_offset;
  uint64_t (*hash_func)(uint8_t *, size_t);
} HopscotchHashtableConfigurable;

#endif //UFSRV_ADT_HOPSCOTCH_HASHTABLE_TYPE_H
