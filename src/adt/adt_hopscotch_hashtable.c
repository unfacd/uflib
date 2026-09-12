/**
 * @defgroup uflib_adt
 */
/*_
 * Copyright (c) 2016 Hirochika Asai <asai@jar.jp>
 * With modifications Copyright (C) 2015-2021 unfacd works
 *
 * Base algorithm: MIT License (Hirochika Asai, 2016).
 * Modifications: GNU Affero General Public License v3.0 (unfacd works, 2015-2024).
 * Combined work is AGPLv3.
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

/** @file
 *  @brief Implementation for hopscotch data structure.
 *
 *  There are two different interfaces defined under this implementation:
 *      - Offset based, whereby a container object is used to derive the key based on known offset size. This mode is
 *      optimised for storing self-contained objects with stable references for the lifetime of the underlying hopscotch
 *      data structure.
 *      - Keyed, whereby the key is stored along with the stored object inside their own "bucket"
 *
 *  @author Hirochika Asai <asai@jar.jp>
 *  @author Ayman Akt
 *  @bug No known bugs.
 */

#include <uflib/adt/adt_hopscotch_hashtable.h>

#define CONFIG_FENCE_PERMISSIONS_KEYLEN								sizeof(unsigned long)
#define CONFIG_DEFAULT_HASHKEY_LEN                    sizeof(unsigned long)

_Static_assert(CONFIG_FENCE_PERMISSIONS_KEYLEN == CONFIG_DEFAULT_HASHKEY_LEN,
    "CONFIG_FENCE_PERMISSIONS_KEYLEN and CONFIG_DEFAULT_HASHKEY_LEN must be equal — "
    "they are used interchangeably across lookup, insert, and remove operations. "
    "Changing one independently of the other would cause silent lookup failures.");

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

static int hopscotch_resize_with_offset(HopscotchHashtable *ht, ItemExtractor extractor_ptr, int delta, size_t key_offset);

/*
 * Initialize the hash table
 */
struct HopscotchHashtable *
hopscotch_init_with_offset(struct HopscotchHashtable *ht, size_t pfactor)
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
hopscotch_lookup_with_offset(struct HopscotchHashtable *ht, ItemExtractor extractor_ptr, uint8_t *key, size_t key_offset)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;

  struct hopscotch_bucket *buckets = ht->buckets;

  sz = 1ULL << ht->pfactor;
  h = _jenkins_hash(key, CONFIG_DEFAULT_HASHKEY_LEN);
  idx = h & (sz - 1);

  if (!buckets[idx].hopinfo) {
      return NULL;
  }

  void  *key_in_container  = NULL,
        *current_item   = NULL;
  for (i=0; i<HOPSCOTCH_HOPINFO_SIZE; i++) {
      if (buckets[idx].hopinfo & (1 << i)) {
        current_item = buckets[idx + i].data;

        if (IS_PRESENT(extractor_ptr)) {
          key_in_container = (char *)(*extractor_ptr)(current_item) + key_offset;
        } else {
          key_in_container = (char *) current_item + key_offset;
        }

        if (0 == memcmp(key, key_in_container, CONFIG_DEFAULT_HASHKEY_LEN)) {
          return current_item; //return unextracted
        }
      }
  }

  return NULL;
}

/**
 * @brief Insert provided object using offset. Refer to @ref hopscotch_remove_with_offset for corresponding removal function.
 * @param ht Pre-allocated and stable reference to a @ref HopscotchHashtable
 * @param extractor_ptr Extractor callback to unmarshall the object. This is optional.
 * @param data Container object to be stored
 * @param key_offset
 * @return
 */
int
hopscotch_insert_with_offset(struct HopscotchHashtable *ht, ItemExtractor extractor_ptr, void *data, size_t key_offset)
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

  struct hopscotch_bucket *buckets = ht->buckets;

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
    if (NULL == buckets[i].data) {
      /* Found an available bucket */
      while (i - idx >= HOPSCOTCH_HOPINFO_SIZE) {
        for (j=1; j < HOPSCOTCH_HOPINFO_SIZE; j++) {
          if (buckets[i - j].hopinfo ) {
            off = __builtin_ctz(buckets[i - j].hopinfo);
            if ( off >= j ) continue;
            buckets[i].data = buckets[i - j + off].data;
            buckets[i - j + off].data = NULL;
            buckets[i - j].hopinfo &= ~(1ULL << off);
            buckets[i - j].hopinfo |= (1ULL << j);
            i = i - j + off;
            break;
          }
        }

        if (j >= HOPSCOTCH_HOPINFO_SIZE) {
          if ((hopscotch_resize_with_offset(ht, extractor_ptr, 1, key_offset)) == -1)	return -1;

          return hopscotch_insert_with_offset(ht, extractor_ptr, data, key_offset);
        }
      }

      off = i - idx;
      buckets[i].data = data;
      buckets[idx].hopinfo |= (1ULL << off);

      return 0;
    }
  }

  return -1;
}

/**
 * Remove an item (in un extracted state if applicable)
 * @param extractor_ptr optional callback to handle a user-define extraction routine to retrieve the item in a state that can be used for key-offsetting
 * (used to compare stored key and provided key to ensure correct item being retrieved)
 * @param key whose hash identifies stored item
 * @param key_offset offset value to locate the index of the key inside stored item
 */
void *
hopscotch_remove_with_offset(struct HopscotchHashtable *ht, ItemExtractor extractor_ptr, uint8_t *key, size_t key_offset)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  void *data;
  void *key_from_container;

  struct hopscotch_bucket *buckets = ht->buckets;

  sz = 1ULL << ht->pfactor;
  h = _jenkins_hash(key, CONFIG_FENCE_PERMISSIONS_KEYLEN);
  idx = h & (sz - 1);

  if (!buckets[idx].hopinfo) {
    return NULL;
  }

  for (i=0; i<HOPSCOTCH_HOPINFO_SIZE; i++) {
    if (buckets[idx].hopinfo & (1 << i)) {
      if (IS_PRESENT(extractor_ptr)) {
        key_from_container = (char *)(*extractor_ptr)(buckets[idx + i].data) + key_offset;
      } else {
        key_from_container = (char *)buckets[idx + i].data + key_offset;
      }

      if (0 == memcmp(key, key_from_container, CONFIG_FENCE_PERMISSIONS_KEYLEN)) {
        data = buckets[idx + i].data;
        buckets[idx].hopinfo &= ~(1ULL << i);
        buckets[idx + i].data = NULL;

        return data; //unextracted item
      }
    }
  }

  return NULL;
}

/*
 * Resize the bucket size of the hash table
 */
static int
hopscotch_resize_with_offset(struct HopscotchHashtable *ht, ItemExtractor extractor_ptr, int delta, size_t key_offset)
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
      ret = hopscotch_insert_with_offset(ht, extractor_ptr, obuckets[i].data, key_offset);
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

size_t
hopscotch_iterator_executor_with_offset(HopscotchHashtable *ht, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr)
{
  size_t completed = 0;

  //only allocated if initialised
  if (likely(IS_PRESENT(ht->buckets))) {
    size_t  i,
            sz;

    struct hopscotch_bucket *buckets = ht->buckets;

    sz = 1ULL << ht->pfactor;

    for (i = 0; i < sz; i++) {
      if (IS_PRESENT(buckets[i].data)) {
        (*executor_ptr)(ctx_ptr, CLIENT_CTX_DATA(buckets[i].data)); //item unextracted
        completed++;
      }
    }
  }

  return completed;
}

void *
hopscotch_iterator_finaliser(HopscotchHashtable *ht, CallbackFinaliser executor_ptr)
{
  //only allocated if initialised
  if (likely(IS_PRESENT(ht->buckets))) {
    size_t  i,
            sz;

    struct hopscotch_bucket *buckets = ht->buckets;
    sz = 1ULL << ht->pfactor;

    for (i = 0; i < sz; i++) {
      if (IS_PRESENT(buckets[i].data)) (*executor_ptr)(CLIENT_CTX_DATA(buckets[i].data)); //item unextracted
    }
  }

  return NULL;
}

//keyed ie non offset-based
static int hopscotch_resize(struct HopscotchHashtable *ht, int delta);

struct HopscotchHashtable *
hopscotch_init(struct HopscotchHashtable *ht, size_t pfactor)
{
  struct hopscotch_bucket_keyed *buckets;

  buckets = malloc(sizeof(struct hopscotch_bucket_keyed) * (1UL << pfactor));//AA size limited to word size of cpu  (2^31 or 2^63)
  if (IS_EMPTY(buckets)) {
    return NULL;
  }

  memset(buckets, 0, sizeof(struct hopscotch_bucket_keyed) * (1UL << pfactor));

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

void *
hopscotch_lookup(struct HopscotchHashtable *ht, intptr_t key)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  uintptr_t key_provided = key;

  sz = 1ULL << ht->pfactor;
  h = _jenkins_hash((uint8_t  *)&key_provided, CONFIG_DEFAULT_HASHKEY_LEN);
  idx = h & (sz - 1);

  struct hopscotch_bucket_keyed *buckets = ht->buckets;

  if (!buckets[idx].hopinfo) {
    return NULL;
  }

  void *current_item = NULL;
  void *current_key = NULL;
  for (i=0; i<HOPSCOTCH_HOPINFO_SIZE; i++) {
    if (buckets[idx].hopinfo & (1 << i)) {
      current_item = buckets[idx + i].data;
      current_key = (void *)buckets[idx + i].key;

      if (0 == memcmp(&key_provided, &current_key, CONFIG_DEFAULT_HASHKEY_LEN)) {
        return current_item;
      }
    }
  }

  return NULL;
}

/**
 * @brief Insert provided object using offset. Refer to @ref hopscotch_remove_with_offset for corresponding removal function.
 * @param ht Pre-allocated and stable reference to a @ref HopscotchHashtable
 * @param extractor_ptr Extractor callback to unmarshall the object. This is optional.
 * @param data Container object to be stored
 * @param key_offset
 * @return
 */
int
hopscotch_insert(struct HopscotchHashtable *ht, uintptr_t key, void *data)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  size_t off;
  size_t j;
  uintptr_t key_provided = key;

  if (unlikely(IS_EMPTY(data)))	return -2;

  sz = 1ULL << ht->pfactor;
  h = _jenkins_hash((uint8_t  *)&key_provided, CONFIG_FENCE_PERMISSIONS_KEYLEN);
  idx = h & (sz - 1);

  struct hopscotch_bucket_keyed *buckets = ht->buckets;

  // Linear probing to find an empty bucket
  for (i=idx; i < sz; i++) {
    if (0 == buckets[i].key) {
      /* Found an available bucket */
      while (i - idx >= HOPSCOTCH_HOPINFO_SIZE) {
        for (j=1; j < HOPSCOTCH_HOPINFO_SIZE; j++) {
          if (buckets[i - j].hopinfo) {
            off = __builtin_ctz(buckets[i - j].hopinfo);
            if ( off >= j ) continue;
            buckets[i].key = buckets[i - j + off].key;
            buckets[i].data = buckets[i - j + off].data;
            buckets[i - j + off].data = NULL;
            buckets[i - j].hopinfo &= ~(1ULL << off);
            buckets[i - j].hopinfo |= (1ULL << j);
            i = i - j + off;
            break;
          }
        }

        if (j >= HOPSCOTCH_HOPINFO_SIZE) {
          if ((hopscotch_resize(ht, 1)) == -1)	return -1;
          return hopscotch_insert(ht, key, data);
        }
      }

      off = i - idx;
      buckets[i].key = key;
      buckets[i].data = data;
      buckets[idx].hopinfo |= (1ULL << off);

      return 0;
    }
  }

  return -1;
}

void *
hopscotch_remove(struct HopscotchHashtable *ht, uintptr_t key)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  uintptr_t key_provided = key;
  void *data;

  sz = 1ULL << ht->pfactor;
  h = _jenkins_hash((uint8_t *)&key_provided, CONFIG_FENCE_PERMISSIONS_KEYLEN);
  idx = h & (sz - 1);

  struct hopscotch_bucket_keyed *buckets = ht->buckets;

  if (!buckets[idx].hopinfo) {
    return NULL;
  }

  uintptr_t key_from_container;
  for (i=0; i<HOPSCOTCH_HOPINFO_SIZE; i++) {
    if (buckets[idx].hopinfo & (1 << i)) {

      key_from_container = buckets[idx + i].key;

      if (0 == memcmp(&key_provided, &key_from_container, CONFIG_FENCE_PERMISSIONS_KEYLEN)) {
        data = buckets[idx + i].data;
        buckets[idx].hopinfo &= ~(1ULL << i);
        buckets[idx + i].data = NULL;

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
hopscotch_resize(struct HopscotchHashtable *ht, int delta)
{
  size_t sz;
  size_t opfactor;
  size_t npfactor;
  ssize_t i;
  struct hopscotch_bucket_keyed *nbuckets;
  struct hopscotch_bucket_keyed *obuckets;
  int ret;

  opfactor = ht->pfactor;
  npfactor = ht->pfactor + delta;
  sz = 1ULL << npfactor;

  syslog (LOG_DEBUG, "%s {pid:'%lu', old_sz:'%lu', new_sz:'%lu'}: Resizing table...", __func__, pthread_self(), opfactor, npfactor);

  nbuckets = malloc(sizeof(struct hopscotch_bucket_keyed) * sz);
  if (NULL == nbuckets) {
    return -1;
  }
  memset(nbuckets, 0, sizeof(struct hopscotch_bucket_keyed) * sz);
  obuckets = ht->buckets;

  ht->buckets = nbuckets;
  ht->pfactor = npfactor;

  struct hopscotch_bucket_keyed *buckets = ht->buckets;

  for (i=0; i<(1ULL << opfactor); i++) {
    if (obuckets[i].key) {
      ret = hopscotch_insert(ht, obuckets[i].key, obuckets[i].data);
      if (ret < 0) {
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

void *
hopscotch_iterator_executor(HopscotchHashtable *ht, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr)
{
  //only allocated if initialised
  if (likely(IS_PRESENT(ht->buckets))) {
    size_t  i,
            sz;

    struct hopscotch_bucket_keyed *buckets = ht->buckets;

    sz = 1ULL << ht->pfactor;

    for (i = 0; i < sz; i++) {
      if (IS_PRESENT(buckets[i].data)) (*executor_ptr)(ctx_ptr, CLIENT_CTX_DATA(buckets[i].data));
    }
  }

  return NULL;
}

// end of keyed

void *
hopscotch_lookup_configurable(HopscotchHashtableConfigurable *htc, uint8_t *key)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  HopscotchHashtable *ht = &(htc->hashtable);
  struct hopscotch_bucket *buckets = ht->buckets;

  sz = 1ULL << ht->pfactor;
  h = (*htc->hash_func)(key, htc->keylen);
  idx = h & (sz - 1);

  if (!buckets[idx].hopinfo) {
    return NULL;
  }

  for (i = 0; i < HOPSCOTCH_HOPINFO_SIZE; i++) {
    if (buckets[idx].hopinfo & (1 << i)) {
      void *key_container = (char *)buckets[idx + i].data + htc->key_offset;
      if (0 == memcmp(key, key_container, htc->keylen)) {
          return buckets[idx + i].data;
      }
    }
  }

  return NULL;
}

void *
hopscotch_iterator_executor_configurable(HopscotchHashtableConfigurable *htc, CallbackExecutor executor_ptr, ClientContextData *ctx_ptr)
{
	size_t i,
				 sz;

	HopscotchHashtable *ht = &(htc->hashtable);
  struct hopscotch_bucket *buckets = ht->buckets;

	sz = 1ULL << ht->pfactor;

	for (i = 0; i < sz; i++) {
		if (IS_PRESENT(buckets[i].data)) (*executor_ptr)(ctx_ptr, CLIENT_CTX_DATA(buckets[i].data));
	}

	return NULL;
}

int
hopscotch_insert_configurable(HopscotchHashtableConfigurable *htc, uint8_t *data)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  size_t off;
  size_t j;
  HopscotchHashtable *ht = &(htc->hashtable);
  struct hopscotch_bucket *buckets = ht->buckets;

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
    if (NULL == buckets[i].data) {
      /* Found an available bucket */
      while (i-idx >= HOPSCOTCH_HOPINFO_SIZE) {
        for (j=1; j<HOPSCOTCH_HOPINFO_SIZE; j++) {
          if (buckets[i - j].hopinfo) {
            off = __builtin_ctz(buckets[i - j].hopinfo);
            if (off >= j) continue;
            buckets[i].data = buckets[i - j + off].data;
            buckets[i - j + off].data = NULL;
            buckets[i - j].hopinfo &= ~(1ULL << off);
            buckets[i - j].hopinfo |= (1ULL << j);
            i = i - j + off;
            break;
          }
        }

        if (j >= HOPSCOTCH_HOPINFO_SIZE) {
          if ((hopscotch_resize_with_offset(ht, ITEM_EXTRACTOR_UNDEFINED, 1, htc->key_offset)) == -1)	return -1;
          return hopscotch_insert_with_offset(ht, NULL, data, htc->key_offset);
        }
      }

      off = i - idx;
      buckets[i].data = data;
      buckets[idx].hopinfo |= (1ULL << off);

      return 0;
    }
  }

  return -1;
}

void *
hopscotch_remove_configurable(HopscotchHashtableConfigurable *htc, uint8_t *key)
{
  uint32_t h;
  size_t idx;
  size_t i;
  size_t sz;
  void *data;
  void *key_from_container;
  HopscotchHashtable *ht = &(htc->hashtable);
  struct hopscotch_bucket *buckets = ht->buckets;

  sz = 1ULL << ht->pfactor;
  h = (*htc->hash_func)(key, htc->keylen);
  idx = h & (sz - 1);

  if ( !buckets[idx].hopinfo ) {
      return NULL;
  }

  for (i=0; i < HOPSCOTCH_HOPINFO_SIZE; i++ ) {
    if (buckets[idx].hopinfo & (1 << i)) {
      key_from_container = (char *)buckets[idx + i].data + htc->key_offset;
      if (0 == memcmp(key, key_from_container, htc->keylen)) {
          data = buckets[idx + i].data;
          buckets[idx].hopinfo &= ~(1ULL << i);
          buckets[idx + i].data = NULL;

          return data;
      }
    }
  }

  return NULL;
}