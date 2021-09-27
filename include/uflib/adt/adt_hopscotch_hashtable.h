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

#ifndef SRC_INCLUDE_ADT_HOPSCOTCH_HASHTABLE_H_
#define SRC_INCLUDE_ADT_HOPSCOTCH_HASHTABLE_H_


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

struct HopscotchHashtable *
hopscotch_init(HopscotchHashtable *, size_t);
void hopscotch_release(HopscotchHashtable *);
void *hopscotch_lookup(HopscotchHashtable *, ItemExtractor extractor_ptr, uint8_t *, size_t);
void *hopscotch_lookup_configurable (HopscotchHashtableConfigurable *htc, uint8_t *key);
int hopscotch_insert(HopscotchHashtable *, ItemExtractor extractor_ptr, void *, size_t);
int hopscotch_insert_configurable (HopscotchHashtableConfigurable *htc, uint8_t *data);
void *hopscotch_remove(HopscotchHashtable *, ItemExtractor extractor_ptr, uint8_t *, size_t);
void *hopscotch_remove_configurable (HopscotchHashtableConfigurable *htc, uint8_t *key);

void *hopscotch_iterator_executor_configurable(HopscotchHashtableConfigurable *htc, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr);
void *hopscotch_iterator_executor (HopscotchHashtable *ht, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr);
void *hopscotch_iterator_finaliser (HopscotchHashtable *ht, CallbackFinaliser executor_ptr);

static inline bool IsHopscotchHashtableAllocated (HopscotchHashtable *ht_ptr) {
	return (IS_PRESENT(ht_ptr->buckets));
}

static inline size_t GetHopscotchHashtableAllocatedSize (HopscotchHashtable *ht_ptr) {
	return 1UL << ht_ptr->pfactor;
}

#endif /* SRC_INCLUDE_ADT_HOPSCOTCH_HASHTABLE_H_ */
