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

#include <uflib/adt/adt_hopscotch_hashtable.h>

#define CONFIG_FENCE_PERMISSIONS_KEYLEN								sizeof(unsigned long)

/*
 * Jenkins Hash Function
 */
static __inline__ uint32_t
_jenkins_hash(uint8_t *key, size_t len)
{
  uint32_t 	hash;
  size_t 		i;

  hash = 0;
  for (i = 0; i < len; i++) {
      hash += key[i];
      hash += (hash << 10);
      hash ^= (hash >> 6);
  }

  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);

  return hash;
}

static int hopscotch_resize(HopscotchHashtable *, ItemExtractor extractor_ptr, int, size_t);

/*
 * Initialize the hash table
 */
struct HopscotchHashtable *
hopscotch_init(struct HopscotchHashtable *ht, size_t pfactor)
{
  struct hopscotch_bucket *buckets;

  buckets = malloc(sizeof(struct hopscotch_bucket) * (1UL << pfactor));//AA size limited to word size of cpu  (2^31 or 2^63)
  if (IS_EMPTY(buckets)) {
      return NULL;
  }

  memset(buckets, 0, sizeof(struct hopscotch_bucket) * (1UL << pfactor));

  if (IS_EMPTY(ht)) {
    ht = malloc(sizeof(struct HopscotchHashtable));
    if (IS_EMPTY(ht)) {
      return NULL;
    }
  }

  ht->pfactor 		= pfactor;
  ht->buckets 		= buckets;

  return ht;
}

/*
 * Release the hash table
 */
void
hopscotch_release(struct HopscotchHashtable *ht)
{
  free(ht->buckets);
  ht->buckets	= NULL;
}

/*
 * Lookup
 */
void *
hopscotch_lookup(struct HopscotchHashtable *ht, ItemExtractor extractor_ptr, uint8_t *key, size_t key_offset)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;

  sz = 1ULL << ht->pfactor;
  h = _jenkins_hash(key, CONFIG_FENCE_PERMISSIONS_KEYLEN);
  idx = h & (sz - 1);

  if (!ht->buckets[idx].hopinfo) {
      return NULL;
  }

  void  *key_container  = NULL,
        *current_item   = NULL;
  for (i=0; i<HOPSCOTCH_HOPINFO_SIZE; i++) {
      if (ht->buckets[idx].hopinfo & (1 << i)) {
        current_item = ht->buckets[idx + i].data;

        if (IS_PRESENT(extractor_ptr)) {
          key_container = (char *)(*extractor_ptr)(current_item) + key_offset;
        } else {
          key_container = (char *) current_item + key_offset;
        }

        if (0 == memcmp(key, key_container, CONFIG_FENCE_PERMISSIONS_KEYLEN)) {
          return current_item; //return unextracted
        }
      }
  }

  return NULL;
}

void *
hopscotch_lookup_configurable (HopscotchHashtableConfigurable *htc, uint8_t *key)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  HopscotchHashtable *ht = &(htc->hashtable);

  sz = 1ULL << ht->pfactor;
  h = (*htc->hash_func)(key, htc->keylen);
  idx = h & (sz - 1);

  if (!ht->buckets[idx].hopinfo) {
    return NULL;
  }

  for (i = 0; i < HOPSCOTCH_HOPINFO_SIZE; i++) {
    if (ht->buckets[idx].hopinfo & (1 << i)) {
      void *key_container = (char *)ht->buckets[idx + i].data + htc->key_offset;
      if (0 == memcmp(key, key_container, htc->keylen)) {
          return ht->buckets[idx + i].data;
      }
    }
  }

  return NULL;
}

void *
hopscotch_iterator_executor_configurable (HopscotchHashtableConfigurable *htc, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr)
{
	size_t i,
				 sz;

	HopscotchHashtable *ht = &(htc->hashtable);
	sz = 1ULL << ht->pfactor;

	for (i = 0; i < sz; i++) {
		if (IS_PRESENT(ht->buckets[i].data)) (*executor_ptr)(ctx_ptr, CLIENT_CTX_DATA(ht->buckets[i].data));
	}

	return NULL;
}

void *
hopscotch_iterator_executor (HopscotchHashtable *ht, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr)
{
  //only allocated if initialised
  if (likely(IS_PRESENT(ht->buckets))) {
    size_t  i,
            sz;

    sz = 1ULL << ht->pfactor;

    for (i = 0; i < sz; i++) {
        if (IS_PRESENT(ht->buckets[i].data)) (*executor_ptr)(ctx_ptr, CLIENT_CTX_DATA(ht->buckets[i].data)); //item unextracted
    }
  }

  return NULL;
}

void *
hopscotch_iterator_finaliser (HopscotchHashtable *ht, CallbackFinaliser executor_ptr)
{
  //only allocated if initialised
  if (likely(IS_PRESENT(ht->buckets))) {
    size_t  i,
            sz;

    sz = 1ULL << ht->pfactor;

    for (i = 0; i < sz; i++) {
      if (IS_PRESENT(ht->buckets[i].data)) (*executor_ptr)(CLIENT_CTX_DATA(ht->buckets[i].data)); //item unextracted
    }
  }

  return NULL;
}

/*
 * Insert an entry to the hash table
 */
int
hopscotch_insert(struct HopscotchHashtable *ht, ItemExtractor extractor_ptr, void *data, size_t key_offset)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  size_t off;
  size_t j;

  if (unlikely(IS_EMPTY(data)))	return -2;

  /* Ensure the key does not exist.  Duplicate keys are not allowed. */
  //AA this rule enforced by caller
//    if ( NULL != hopscotch_lookup(ht, key) ) {
//        /* The key already exists. */
//        return -1;
//    }

  void *key_contained = NULL;
  if (IS_PRESENT(extractor_ptr)) {
    key_contained = (char *)(*extractor_ptr)(data) + key_offset;
  } else {
    key_contained = (char *)data + key_offset;
  }

  sz = 1ULL << ht->pfactor;
  h = _jenkins_hash(key_contained, CONFIG_FENCE_PERMISSIONS_KEYLEN);
  idx = h & (sz - 1);

  // Linear probing to find an empty bucket
  for (i=idx; i < sz; i++) {
    if (NULL == ht->buckets[i].data) {
      /* Found an available bucket */
      while (i - idx >= HOPSCOTCH_HOPINFO_SIZE) {
        for (j=1; j < HOPSCOTCH_HOPINFO_SIZE; j++) {
          if (ht->buckets[i - j].hopinfo ) {
            off = __builtin_ctz(ht->buckets[i - j].hopinfo);
            if ( off >= j ) continue;
            ht->buckets[i].data = ht->buckets[i - j + off].data;
            ht->buckets[i - j + off].data = NULL;
            ht->buckets[i - j].hopinfo &= ~(1ULL << off);
            ht->buckets[i - j].hopinfo |= (1ULL << j);
            i = i - j + off;
            break;
          }
        }

        if (j >= HOPSCOTCH_HOPINFO_SIZE) {
          if ((hopscotch_resize(ht, extractor_ptr, 1, key_offset)) == -1)	return -1;

          return hopscotch_insert(ht, extractor_ptr, data, key_offset);
        }
      }

      off = i - idx;
      ht->buckets[i].data = data;
      ht->buckets[idx].hopinfo |= (1ULL << off);

      return 0;
    }
  }

  return -1;
}

int
hopscotch_insert_configurable (HopscotchHashtableConfigurable *htc, uint8_t *data)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  size_t off;
  size_t j;
  HopscotchHashtable *ht = &(htc->hashtable);

  if (unlikely(IS_EMPTY(data)))	return -2;

  /* Ensure the key does not exist.  Duplicate keys are not allowed. */
  //AA this rule enforced by caller
//    if ( NULL != hopscotch_lookup(ht, key) ) {
//        /* The key already exists. */
//        return -1;
//    }

  void *key_contained = (char *)data + htc->key_offset;
  sz = 1ULL << ht->pfactor;

  h = (*htc->hash_func)(data, htc->keylen);
  idx = h & (sz - 1);

  // Linear probing to find an empty bucket
  for (i=idx; i < sz; i++) {
    if (NULL == ht->buckets[i].data) {
      /* Found an available bucket */
      while (i-idx >= HOPSCOTCH_HOPINFO_SIZE) {
        for (j=1; j<HOPSCOTCH_HOPINFO_SIZE; j++) {
          if (ht->buckets[i - j].hopinfo) {
            off = __builtin_ctz(ht->buckets[i - j].hopinfo);
            if (off >= j) continue;
            ht->buckets[i].data = ht->buckets[i - j + off].data;
            ht->buckets[i - j + off].data = NULL;
            ht->buckets[i - j].hopinfo &= ~(1ULL << off);
            ht->buckets[i - j].hopinfo |= (1ULL << j);
            i = i - j + off;
            break;
          }
        }

        if (j>=HOPSCOTCH_HOPINFO_SIZE) {
          if ((hopscotch_resize(ht, ITEM_EXTRACTOR_UNDEFINED, 1, htc->key_offset))==-1)	return -1;
          return hopscotch_insert(ht, NULL, data, htc->key_offset);
        }
      }

      off = i - idx;
//            ht->buckets[i].key = key;
      ht->buckets[i].data = data;
      ht->buckets[idx].hopinfo |= (1ULL << off);

      return 0;
    }
  }

  return -1;
}

/*
 * Remove an item (in un extracted state if applicable)
 */
void *
hopscotch_remove(struct HopscotchHashtable *ht, ItemExtractor extractor_ptr, uint8_t *key, size_t key_offset)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  void *data;
  void *key_from_container;

  sz = 1ULL << ht->pfactor;
  h = _jenkins_hash(key, CONFIG_FENCE_PERMISSIONS_KEYLEN);
  idx = h & (sz - 1);

  if (!ht->buckets[idx].hopinfo) {
    return NULL;
  }

  for (i=0; i<HOPSCOTCH_HOPINFO_SIZE; i++) {
    if (ht->buckets[idx].hopinfo & (1 << i)) {
      if (IS_PRESENT(extractor_ptr)) {
        key_from_container = (char *)(*extractor_ptr)(ht->buckets[idx + i].data) + key_offset;
      } else {
        key_from_container = (char *)ht->buckets[idx + i].data + key_offset;
      }

      if (0 == memcmp(key, key_from_container, CONFIG_FENCE_PERMISSIONS_KEYLEN)) {
        data = ht->buckets[idx + i].data;
        ht->buckets[idx].hopinfo &= ~(1ULL << i);
        ht->buckets[idx + i].data = NULL;

        return data; //unextracted item
      }
    }
  }

  return NULL;
}

void *
hopscotch_remove_configurable (HopscotchHashtableConfigurable *htc, uint8_t *key)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  void *data;
  void *key_from_container;
  HopscotchHashtable *ht = &(htc->hashtable);

  sz = 1ULL << ht->pfactor;
  h = (*htc->hash_func)(key, htc->keylen);
  idx = h & (sz - 1);

  if ( !ht->buckets[idx].hopinfo ) {
      return NULL;
  }

  for (i=0; i < HOPSCOTCH_HOPINFO_SIZE; i++ ) {
    if (ht->buckets[idx].hopinfo & (1 << i)) {
      key_from_container = (char *)ht->buckets[idx + i].data + htc->key_offset;
      if (0 == memcmp(key, key_from_container, htc->keylen)) {
          data = ht->buckets[idx + i].data;
          ht->buckets[idx].hopinfo &= ~(1ULL << i);
          ht->buckets[idx + i].data = NULL;

          return data;
      }
    }
  }

  return NULL;
}

/*
 * Resize the bucket size of the hash table
 */
static int
hopscotch_resize(struct HopscotchHashtable *ht, ItemExtractor extractor_ptr, int delta, size_t key_offset)
{
  size_t sz;
  size_t opfactor;
  size_t npfactor;
  ssize_t i;
  struct hopscotch_bucket *nbuckets;
  struct hopscotch_bucket *obuckets;
  int ret;

  opfactor = ht->pfactor;
  npfactor = ht->pfactor + delta;
  sz = 1ULL << npfactor;

  syslog (LOG_DEBUG, "%s {pid:'%lu', old_sz:'%lu', new_sz:'%lu'}: Resizing table...", __func__, pthread_self(), opfactor, npfactor);

  nbuckets = malloc(sizeof(struct hopscotch_bucket) * sz);
  if (NULL == nbuckets) {
      return -1;
  }
  memset(nbuckets, 0, sizeof(struct hopscotch_bucket) * sz);
  obuckets = ht->buckets;

  ht->buckets = nbuckets;
  ht->pfactor = npfactor;

  for (i=0; i<(1ULL << opfactor); i++) {
    if ( obuckets[i].data ) {
      ret = hopscotch_insert(ht, extractor_ptr, obuckets[i].data, key_offset);
      if (ret<0) {
        ht->buckets = obuckets;
        ht->pfactor = opfactor;
        free(nbuckets);

        return -1;
      }
    }
  }

  free(obuckets);

  return 0;
}