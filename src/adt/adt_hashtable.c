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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <string.h>
#include <errno.h>
#include <uflib/adt/adt_hashtable.h>
#include <uflib/optional/ufoptional.h>
#include <uflib/utils.h>
#include <uflib/utils_json.h>
#include <syslog.h>

static unsigned int _ComputeHash(HashTable *, const void *item_key);
static unsigned int _ComputeHashFromItem(HashTable *, void *item_key);
static int          ExpandTable(HashTable *);
static int          IsItemEqualToKey(HashTable *, void *, const void *);
static void *       _RemoveFromHash(HashTable *hash, void *item, bool flag_lock_it,
                                    void (*    reference_decrementor_callback)(RecyclerClientData *, int));
static void *             _DoAddItem(HashTable *hasht_ptr, void *item);
static void *             _IsItemHashed(HashTable *hash, const void *data);
static ClientContextData *_DefaultItemExtractor(ItemContainer *);
/* FIX: non-expanding re-insert for remove-compaction only (must not grow table mid-walk). */
static void               _ReinsertForCompact(HashTable *hash, void *item_container);

/**
 * Key configuration is fixed here and does not change afterwards. Two independent
 * dimensions: the offset of the key field within the *extracted* item, and whether that
 * field holds a pointer to the key or the key inline. Container extraction is a third,
 * separate dimension: the callback below.
 *
 * For a locking version see HashTableLockingInstantiate()
 * @param ht_ptr Pre-allocated HasTable object
 * @param offset Offset of the key field within the extracted item, from offsetof();
 *               pass HASHTABLE_ITEM_CONTAINER_OFFSET_ZERO only when the item is itself the key
 * @param key_size sizeof(key) for a fixed-width key (eg sizeof(unsigned long));
 *                 KEY_SIZE_ZERO for a NUL-terminated string key
 * @param key_is_ptr HASHTABLE_KEY_IS_PTR when the key field holds a pointer to the key
 *                 (char *name); HASHTABLE_KEY_IS_INLINE when the key is stored inline
 *                 (char name[N], unsigned long id). Governs key indirection only - it says
 *                 nothing about whether callers pass containers
 * @param name Descriptive name for the HashTable. If ignored, the table will be called "AnonymousTable"
 * @param item_extractor_callback Maps a caller-supplied container to the item it holds.
 *                 Pass HASHTABLE_DEFAULT_EXTRACTOR when containers and items are the same object
 * @return HashTable * initialised as per provided parameters.
 */
PUBLIC_API HashTable *
HashTableInstantiate(HashTable *   ht_ptr, int offset, int key_size, long key_is_ptr, const char *name,
                     ItemExtractor item_extractor_callback)
{
  HashTable *result = NULL;
  if (IS_EMPTY(ht_ptr)) {
    result = (HashTable*)calloc(1, sizeof(HashTable));
    if (IS_EMPTY(result)) return NULL;
  }
  else {
    result = ht_ptr;
    /* FIX: if re-instantiating an existing object, free previous strdup'd name to avoid leak. */
    if (IS_PRESENT(result->table_name)) {
      free((void*)result->table_name);
      result->table_name = NULL;
    }
    /* FIX: if it was a locking table, destroy the old lock before re-init (caller must not hold it). */
    if (result->flag_locking) {
      pthread_rwlock_destroy(&(result->hashtable_rwlock));
      result->flag_locking = 0;
    }
    if (IS_PRESENT(result->fTable)) {
      free(result->fTable);
      result->fTable = NULL;
    }
  }

  result->fTable                  = NULL;
  result->fTableSize              = 0;
  result->fNumEntries             = 0;
  result->fKeyOffset              = offset;
  result->fKeySize                = key_size;
  result->fKeyIsPtr               = key_is_ptr;
  result->ref_count               = 0;
  result->table_name              = name ? strdup(name) : strdup("AnonymousTable");
  result->flag_resizable          = 1;
  result->flag_unique_keys        = 0;
  result->item_extractor_callback = item_extractor_callback;
  if (IS_EMPTY(result->item_extractor_callback)) result->item_extractor_callback = _DefaultItemExtractor;

  return result;
}


/**
 * 	@brief Initialise the locking mutex and sets the locking property of the hash table. See detailed documentation under
 * 	HashTableInstantiate().
 */
HashTable *
HashTableLockingInstantiate(HashTable *   ht_ptr_in, int offset, int key_size, long key_is_ptr, const char *name,
                            ItemExtractor item_extractor_callback)
{
  HashTable *ht_ptr = NULL;
  if ((ht_ptr = HashTableInstantiate(ht_ptr_in, offset, key_size, key_is_ptr, name, item_extractor_callback))) {
    int ret = pthread_rwlock_init(&(ht_ptr->hashtable_rwlock), NULL);
    if (ret != 0) {
      char error_str[250];
      strerror_r(errno, error_str, 250);

      syslog(LOG_ERR, "%s: ERROR (errno:'%d'): COULD NOT INITIALISE hashtable_rwlock: error: '%s'...", __func__, errno,
             error_str);

      /* we were responsible for allocating the HashTable object, so we free it */
      if (IS_EMPTY(ht_ptr_in)) {
        free((void*)ht_ptr->table_name);
        free(ht_ptr);
      }

      return NULL;
    }

    ht_ptr->flag_locking = 1;

    return ht_ptr;
  }

  return NULL;
}

/**
 * 	@brief Opt a table into unique-key semantics. Call before the table is used concurrently.
 */
PUBLIC_API HashTable *
HashTableSetUniqueKeys(HashTable *hash, bool unique_keys)
{
  if (unlikely(IS_EMPTY(hash))) return NULL;

  hash->flag_unique_keys = unique_keys ? 1 : 0;

  return hash;
}

void
HashTableDestruct(HashTable *hash, bool is_self_destruct)
{
  if (IS_EMPTY(hash)) return;

  if (IS_PRESENT(hash->fTable)) {
    free(hash->fTable);
    hash->fTable = NULL;
  }

  free((void*)hash->table_name); /* strdup'd by HashTableInstantiate — safe on NULL */
  hash->table_name = NULL;

  if (hash->flag_locking) {
    pthread_rwlock_destroy(&(hash->hashtable_rwlock));
    hash->flag_locking = 0;
  }

  if (is_self_destruct) {
    free(hash);
  }
}

/* readlock safe
 * should be wrapped in write lock in the calling function
 */
/**
 * @brief Calculate hash value from a given key.
 * @param hasht_ptr
 * @param item_key retrieved based on known offset in item container (eg. session_id)
 * @return
 */
inline static unsigned int
_ComputeHash(HashTable *hasht_ptr, const void *item_key)
{
  unsigned int         table_offset;
  unsigned int         i;
  const unsigned char *keyPtr = (unsigned char*)item_key;
  int                  key_sz = hasht_ptr->fKeySize;

  if (unlikely(IS_EMPTY(item_key))) return 0;

  /* empty table must not modulo by (size-1); callers already guard fTableSize==0 on lookup,
   * but keep defensive behaviour if expand has not yet run. */
  if (unlikely(hasht_ptr->fTableSize == 0)) return 0;

  table_offset = 0;
  if (key_sz == 0) key_sz = (int)strlen((char*)item_key);

  for (i = 0; i < (unsigned)key_sz; i++)
    table_offset = (table_offset ^ keyPtr[i] ^ (keyPtr[i] << 1) ^ (keyPtr[i] << 8)) + (keyPtr[i] << (keyPtr[i] % 7));

  table_offset = table_offset % (hasht_ptr->fTableSize - 1);

#ifdef __UF_FULLDEBUG
  syslog(LOG_INFO, "%s: %s: computed hashoffset: '%i", __func__, hasht_ptr->table_name, table_offset);
#endif

  return table_offset;
}

/**
 * @brief readlock safe
 * @param item_container user data container (struct) which contains a member whose value is going to be used as key. The container must be in 'extracted' state.
 * offset defined in the hashtable is used to locate the contained item to eb used as keu. e.g Session -> session_id.
 * Where the item is not contained in a struct (fKeyIsPtr = false), this will be the key directly.
 * @return hash value to be used for indexing
 */
inline static unsigned int
_ComputeHashFromItem(HashTable *hasht_ptr, void *item_container)
{
  void *key;

  if (hasht_ptr->fKeyIsPtr) key = *(void**)((char*)item_container + hasht_ptr->fKeyOffset);
  else key                      = (char*)item_container + hasht_ptr->fKeyOffset;

  return _ComputeHash(hasht_ptr, (const void*)key);
}

/* readlock safe */
/**
 * 	@brief: Given a hash key, verify if value stored against it matches a given value.
 * 	This is reverse verification
 * 	 confirms that the key-value pair are the original unit that was used to compute this hash slot, as multiple keys can be used
 * 	 with differing value, we need a way to associate the different combination of key-value pairs.
 *  @param key_container must be in extracted state
 */
inline static int
IsItemEqualToKey(HashTable *hash, void *item_container, const void *item_key)
{
  void *key_from_container;
  int   keySize = hash->fKeySize;

  if (hash->fKeyIsPtr) key_from_container = *(void**)((char*)item_container + hash->fKeyOffset);
  else key_from_container                 = (char*)item_container + hash->fKeyOffset;

  if (key_from_container == NULL) return 0;

  if (keySize == 0) return (strcmp(key_from_container, item_key) == 0);

  return (memcmp(key_from_container, item_key, hash->fKeySize) == 0);
}

/* readlock safe: caller holds the table's write lock.
 * Compares the key values of two containers, not the containers and not their extractor output.
 */
inline static bool
IsSameKey(HashTable *hash, void *item_container_a, void *item_container_b)
{
  void *key_a = hash->fKeyIsPtr ?
                  *(void**)((char*)item_container_a + hash->fKeyOffset) :
                  (char*)item_container_a + hash->fKeyOffset;

  if (IS_EMPTY(key_a)) return false;

  return (IsItemEqualToKey(hash, item_container_b, key_a) != 0);
}

/* readlock safe: caller holds the table's write lock.
 * The duplicate test used by every insertion path: key values when the table is unique-keyed,
 * the extracted item pointer otherwise. Both arguments are EXTRACTED ITEMS, never containers -
 * the non-unique branch compares pointers, the unique branch reads the key field through them.
 */
inline static bool
IsDuplicateItem(HashTable *hash, void *stored_container, void *item_container)
{
  if (hash->flag_unique_keys) return IsSameKey(hash, stored_container, item_container);

  return (HASHTABLE_EXTRACT_ITEM(hash, stored_container) == item_container);
}

/* readlock UNSAFE
 * should be wrapped in write lock in the calling environment
 * return 0 on success, -1 on allocation failure (was silent void return).
 */
inline static int
ExpandTable(HashTable *ht_ptr)
{
  int    i;
  size_t oldSize = ht_ptr->fTableSize;

  /* Guard against silent wrap when oldSize > SIZE_MAX/2.
   * Unreachable in practice (would require ~45 doublings from 65521
   * on 64-bit before OOM) but correct. */
  if (oldSize > SIZE_MAX / 2) return -1;

  size_t newSize = oldSize * 2;

  if (ht_ptr->flag_resizable == 0) {
    /* special condition for non-resizable tables, allowing for the size to be set separately from default */
    if (ht_ptr->max_size > 0) newSize = ht_ptr->max_size;
    else goto compute_size; //TODO: this is dumb: set newSize to zero and remove 'else' below and test if (newsize>0)
  }
  else {
compute_size:
    if (newSize == 0) newSize = _CONFIGDEFAULT_HASHTABLE_EXPANSION_SZ; /* bootstrapping the hashtable */
    else newSize              = GetNextPrimeNumber(2 * oldSize);
  }

  void **newTable = (void**)calloc(newSize, sizeof(void*));

  /* propagate allocation failure so callers do not assume growth happened. */
  if (unlikely(IS_EMPTY(newTable))) return -1;

#if UF_DEBUG_BUILD
  syslog(LOG_DEBUG, "%s (pid='%lu'): %s: Current size: '%lu'. New size: '%lu'", __func__, pthread_self(),
         ht_ptr->table_name, ht_ptr->fTableSize, newSize);
#endif

  ht_ptr->fTableSize = newSize;

  if (ht_ptr->fTable != NULL) {
    for (i = 0; i < (int)oldSize; i++) {
      if (ht_ptr->fTable[i] != NULL) {
        void *item          = ht_ptr->fTable[i];
        int   newHashOffset = (int)_ComputeHashFromItem(ht_ptr, HASHTABLE_EXTRACT_ITEM(ht_ptr, item));
        while (newTable[newHashOffset] != NULL) newHashOffset = (newHashOffset + 1) % (int)newSize;

        newTable[newHashOffset] = item;
      }
    }

    free(ht_ptr->fTable);
  }

  ht_ptr->fTable = newTable;
  return 0;
}

/*
 * Compaction re-insert must NOT expand the table.
 * Expanding mid-walk invalidates indices and leaves the compaction loop incorrect.
 * After a successful remove there is at least one free slot, so a non-growing
 * linear probe is always sufficient.
 */
static void
_ReinsertForCompact(HashTable *hash, void *item_container)
{
  void *       item_extracted = HASHTABLE_EXTRACT_ITEM(hash, item_container);
  unsigned int hashOffset     = _ComputeHashFromItem(hash, item_extracted);

  while (hash->fTable[hashOffset] != NULL) {
    /* Same extracted pointer already present (should not happen during compact). */
    if (HASHTABLE_EXTRACT_ITEM(hash, hash->fTable[hashOffset]) == item_extracted) return;
    hashOffset = (hashOffset + 1) % hash->fTableSize;
  }

  hash->fTable[hashOffset] = item_container;
  hash->fNumEntries++;
}

/* TBD same as AddToHash but assumes table already locked and used in the context of reorganising EXISTING ITEMS NOT ADDING TOTALLY NEW ONES
 * NOT SUBJECT TO RESIZING FLAG TEST
 * FIX: kept for any residual macro use; compaction now calls _ReinsertForCompact instead.
 */
#define ADDTOHASH \
		if (hash->fNumEntries * 3 >= hash->fTableSize * 2)	ExpandTable(hash);\
			\
			void *item_extracted = HASHTABLE_EXTRACT_ITEM(hash, item);\
			int hashOffset2 = _ComputeHashFromItem(hash, item_extracted);\
			/*syslog (LOG_DEBUG, "%s: %s: using index offset:'%u'",   __func__, hash->table_name, hashOffset2);*/\
\
			unsigned counter = 0;\
			while (hash->fTable[hashOffset2] != NULL) {\
				counter++;\
				if (HASHTABLE_EXTRACT_ITEM(hash, hash->fTable[hashOffset2]) == item_extracted) {\
					syslog (LOG_DEBUG, "%s (o:'%p', counter: '%u'): %s: item is already in HashTable: index='%i'", __func__, item_extracted, counter, hash->table_name, hashOffset2);\
					/*HashTable_UnLock (hash);*/\
					/*break;*/\
				}\
\
				hashOffset2 = (hashOffset2 + 1) % hash->fTableSize;\
				syslog(LOG_DEBUG, "%s (counter: '%u'): %s: updating index offset: new offset: '%i'", __func__, counter, hash->table_name, hashOffset2);\
			}\
\
			syslog (LOG_DEBUG, "%s (counter: '%u'): %s: final index offset: '%i'. Number of entries in HashTable: '%ld'", __func__, counter, hash->table_name, hashOffset2, hash->fNumEntries+1);\
			hash->fTable[hashOffset2] = item;\
			hash->fNumEntries++;

#define	RESIZABLE_TABLE_AT_CAPACITY(x)	((x)->fNumEntries * 3 >= (x)->fTableSize * 2)
#define	FIXEDSIZE_TABLE_AT_CAPACITY(x)	((x)->fNumEntries == (x)->fTableSize && (x)->fTableSize > 0)

/*
 * 	@brief: All capacity checks must have been peformed
 * 	@locked RW_LOCKED hasht_ptr:
 */
static void *
_DoAddItem(HashTable *hasht_ptr, void *item_container)
{
  if (unlikely(IS_EMPTY(item_container))) return NULL;

  void *item_container_extracted = HASHTABLE_EXTRACT_ITEM(hasht_ptr, item_container);
  if (unlikely(IS_EMPTY(item_container_extracted))) return NULL;

  unsigned int hashOffset = _ComputeHashFromItem(hasht_ptr, item_container_extracted);

#if UF_DEBUG_BUILD_L3
  syslog(LOG_DEBUG, "%s {pid:'%lu'}: %s: using index offset:'%u'", __func__, pthread_self(), hasht_ptr->table_name,
         hashOffset);
#endif

  unsigned counter = 0;
  while (hasht_ptr->fTable[hashOffset] != NULL) {
    counter++;
    if (unlikely(counter > hasht_ptr->fTableSize)) return NULL; //table is full: nowhere to insert

    void *stored_item_container = hasht_ptr->fTable[hashOffset];

    /* fTable holds containers: resolve the stored one before any key read. */
    if (IsDuplicateItem(hasht_ptr, HASHTABLE_EXTRACT_ITEM(hasht_ptr, stored_item_container),
                        item_container_extracted)) {
#if UF_DEBUG_BUILD
      syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', counter: '%u') %s: item is already in HashTable: index='%i'", __func__,
             pthread_self(), item_container_extracted, counter, hasht_ptr->table_name, hashOffset);
#endif

      return hasht_ptr->flag_unique_keys ? stored_item_container : item_container;
    }

    hashOffset = (hashOffset + 1) % hasht_ptr->fTableSize;

#if UF_DEBUG_BUILD
    syslog(LOG_DEBUG, "%s {pid:'%lu', counter: '%u'} %s: updating index offset: new offset: '%i'", __func__,
           pthread_self(), counter, hasht_ptr->table_name, hashOffset);
#endif
  }

#if UF_DEBUG_BUILD
  syslog(LOG_DEBUG, "%s {pid:'%lu', counter:'%u'} %s: final index offset: '%i'. Number of entries in HashTable: '%ld'",
         __func__, pthread_self(), counter, hasht_ptr->table_name, hashOffset, hasht_ptr->fNumEntries + 1);
#endif

  hasht_ptr->fTable[hashOffset] = item_container;
  hasht_ptr->fNumEntries++;

  return item_container;
}

/* OPTIK path removed — CONFIG_USE_OPTIK_LOCK is being phased out.
 * Only the pthread_rwlock path remains. */

/* Every insert verb shares this body. The policy carries what makes them different:
 * how capacity is recovered, whether a reference or eviction callback applies, and which
 * of the two claim reports the caller wants back.
 * Invariants the funnel keeps regardless of policy: the extractor runs under the write lock
 * before any growth, the duplicate check runs before any eviction, and reference callbacks
 * fire only when the entry count grew. */
typedef enum InsertCapacity
{
  INSERT_CAPACITY_GROW, /* expand, or refuse a full fixed-size table */
  INSERT_CAPACITY_EVICT /* let the evictor name a victim before giving up */
} InsertCapacity;

typedef struct InsertPolicy
{
  InsertCapacity capacity;
  const char *   capacity_refusal_label;
  void *(*       evictor)(ContextData *);
  ContextData *  evictor_ctx;
  void **        evicted_out;
  void (*        on_accepted)(RecyclerClientData *, int);
  void (*        on_evicted)(RecyclerClientData *, int);
  bool *         is_added;            /* true when the key was already hashed */
  bool *         was_inserted;        /* true when this container became resident */
  void **        resident_out;        /* who holds the key afterwards */
  bool           replace_on_conflict; /* a unique-key incumbent gives way to this container */
} InsertPolicy;

static void *
_AddToHashInternal(HashTable *hash, const void *item_key, void *item_container, const InsertPolicy *policy)
{
  if (unlikely(IS_EMPTY(hash) || IS_EMPTY(item_container))) return NULL;

  if (hash->flag_locking) if ((HashTable_WrLock(hash, 0)) != 0) return NULL;

  /* the extractor is table state: run it only under the write lock, before any growth */
  if (unlikely(IS_EMPTY(HASHTABLE_EXTRACT_ITEM(hash, item_container)))) {
    if (hash->flag_locking) HashTable_UnLock(hash);
    return NULL;
  }

  void *hashed_item_container = item_container;
  long  entries_before        = 0;
  bool  at_capacity           = false;
  bool  already_hashed        = false;

  if (hash->flag_resizable) {
    if (RESIZABLE_TABLE_AT_CAPACITY(hash)) {
      if (policy->capacity == INSERT_CAPACITY_EVICT && hash->fNumEntries > 0) {
        at_capacity = true;
      }
      else if (ExpandTable(hash) != 0) {
        if (hash->flag_locking) HashTable_UnLock(hash);
        return NULL;
      }
    }
  }
  else {
    if (FIXEDSIZE_TABLE_AT_CAPACITY(hash)) {
      if (policy->capacity == INSERT_CAPACITY_EVICT) {
        at_capacity = true;
      }
      else {
        syslog(LOG_ERR, "%s: %s (table_sz:'%lu'): ERROR: %s... Number of entries in HashTable: '%ld'", __func__,
               hash->table_name, hash->fTableSize, policy->capacity_refusal_label, hash->fNumEntries);
        if (hash->flag_locking) HashTable_UnLock(hash);

        return NULL;
      }
    }
    else if (policy->capacity == INSERT_CAPACITY_EVICT) {
      if (hash->fNumEntries == hash->fTableSize && hash->fTableSize == 0) {
        if (ExpandTable(hash) != 0) { //bootstrap condition for non resizable
          if (hash->flag_locking) HashTable_UnLock(hash);
          return NULL;
        }
      }
    }
    else if (ExpandTable(hash) != 0) { //bootstrap condition for non resizable
      if (hash->flag_locking) HashTable_UnLock(hash);
      return NULL;
    }
  }

  if (at_capacity) {
    void *hashed_existing_item = IS_PRESENT(item_key) ? _IsItemHashed(hash, item_key) : NULL;

    if (IS_PRESENT(hashed_existing_item)) { //can be inferred if item_key==NULL
      if (hash->flag_unique_keys) hashed_item_container = hashed_existing_item;
      already_hashed = true;
#if UF_DEBUG_BUILD
      syslog(LOG_DEBUG, "%s (pid:'%lu'): %s: NOTICE: ITEM ALREADY HASHED: NO EVICTION FROM LIST IS NECESSARY", __func__,
             pthread_self(), hash->table_name);
#endif
      goto report;
    }

    if (IS_PRESENT(policy->evictor)) {
      /* WARNING: callback runs under write lock — must not re-enter this table. */
      void *evicted_item = (*policy->evictor)(policy->evictor_ctx);
      if (IS_PRESENT(evicted_item)) {
        _RemoveFromHash(hash, evicted_item, false, policy->on_evicted);
        if (IS_PRESENT(policy->evicted_out)) *policy->evicted_out = evicted_item;
      }
    }
  }

  entries_before             = hash->fNumEntries;
  void *added_item_container = _DoAddItem(hash, item_container);

  /* nothing was installed and nothing was found: leave the caller's reports untouched */
  if (unlikely(IS_EMPTY(added_item_container))) goto return_unlock;

  hashed_item_container = added_item_container;

  /* a unique-key conflict may be resolved in the caller's favour: the incumbent gives way */
  if (policy->replace_on_conflict && hash->fNumEntries == entries_before && added_item_container != item_container) {
    _RemoveFromHash(hash, added_item_container, false, policy->on_evicted);
    if (IS_PRESENT(policy->evicted_out)) *policy->evicted_out = added_item_container;

    entries_before        = hash->fNumEntries;
    hashed_item_container = _DoAddItem(hash, item_container);

    if (unlikely(IS_EMPTY(hashed_item_container))) goto return_unlock;
  }

  already_hashed = (hash->fNumEntries == entries_before);

  if (IS_PRESENT(policy->on_accepted) && !already_hashed)
    (*policy->on_accepted)((RecyclerClientData*)hashed_item_container, 1);

report:
  if (IS_PRESENT(policy->is_added)) *policy->is_added = already_hashed;
  if (IS_PRESENT(policy->was_inserted)) *policy->was_inserted = !already_hashed;
  if (IS_PRESENT(policy->resident_out)) *policy->resident_out = hashed_item_container;

return_unlock:
  if (hash->flag_locking) HashTable_UnLock(hash);

  return hashed_item_container;
}

PUBLIC_API void *
AddToHash(HashTable *hash, void *item_container)
{
  return AddToHashWithIndication(hash, item_container, HASHTABLE_DONT_INDICATE_IS_ADDED);
}

/**
 * @brief Attempts to add an item to the underlying hashtable: if the items is already hashed, \p is_added will be set to true.
 * @param hash Pre-allocated and configured Hashtable
 * @param item_container Container for item to be hashed.
 * @param is_added If the caller wants to know if the item was added or was already in the hash, pass a pointer.
 * @return
 */
PUBLIC_API void *
AddToHashWithIndication(HashTable *hash, void *item_container, bool *is_added)
{
  InsertPolicy policy = {
    .capacity = INSERT_CAPACITY_GROW, .capacity_refusal_label = "NON RESIZABLE TABLE REACHED CAPACITY",
    .is_added = is_added
  };

  return _AddToHashInternal(hash, NULL, item_container, &policy);
}

/**
 * @brief Insert into a table opted into unique keys, telling the caller who holds the key afterwards.
 * @param hash Table configured with HashTableSetUniqueKeys(hash, true).
 * @param item_container Container to insert.
 * @param was_inserted Set to true only when \p item_container itself became the resident container.
 * @param resident_container Set to the container holding the key afterwards: \p item_container on a
 *        successful claim, the incumbent on a refusal, NULL when nothing could be attempted.
 * @return Resident container, or NULL when the table is not unique-keyed or the insert failed.
 *
 * A refusal is not an error: the caller compares the resident container against the one it offered
 * to decide whether its own instance was stored or discarded.
 */
PUBLIC_API void *
AddToHashUniquely(HashTable *hash, void *item_container, bool *was_inserted, void **resident_container)
{
  if (IS_PRESENT(was_inserted)) *was_inserted = false;
  if (IS_PRESENT(resident_container)) *resident_container = NULL;

  if (unlikely(IS_EMPTY(hash) || IS_EMPTY(item_container))) return NULL;

  if (unlikely(!HASHTABLE_ISUNIQUEKEYS(hash))) {
    syslog(LOG_ERR, "%s: %s: ERROR: TABLE IS NOT UNIQUE-KEYED: CALL HashTableSetUniqueKeys() AT CONSTRUCTION", __func__,
           hash->table_name);

    return NULL;
  }

  InsertPolicy policy = {
    .capacity     = INSERT_CAPACITY_GROW, .capacity_refusal_label = "NON RESIZABLE TABLE REACHED CAPACITY",
    .was_inserted = was_inserted, .resident_out                   = resident_container
  };

  return _AddToHashInternal(hash, NULL, item_container, &policy);
}

/**
 * @brief Insert into a unique-key table, displacing whatever instance holds the key.
 * @param hash Table configured with HashTableSetUniqueKeys(hash, true).
 * @param item_container Container to insert.
 * @param replaced_container Set to the displaced instance, or NULL when the key was free or already held
 *        by \p item_container.
 * @return Resident container, or NULL when the table is not unique-keyed or the insert failed.
 *
 * Use where an authority outside the table decides which instance must hold the key — a backend write
 * that named this container, for instance — so the incumbent is stale rather than authoritative.
 */
PUBLIC_API void *
AddToHashReplacing(HashTable *hash, void *item_container, void **replaced_container)
{
  if (IS_PRESENT(replaced_container)) *replaced_container = NULL;

  if (unlikely(IS_EMPTY(hash) || IS_EMPTY(item_container))) return NULL;

  if (unlikely(!HASHTABLE_ISUNIQUEKEYS(hash))) {
    syslog(LOG_ERR, "%s: %s: ERROR: TABLE IS NOT UNIQUE-KEYED: CALL HashTableSetUniqueKeys() AT CONSTRUCTION", __func__,
           hash->table_name);

    return NULL;
  }

  InsertPolicy policy = {
    .capacity    = INSERT_CAPACITY_GROW, .capacity_refusal_label = "NON RESIZABLE TABLE REACHED CAPACITY",
    .evicted_out = replaced_container, .replace_on_conflict      = true
  };

  return _AddToHashInternal(hash, NULL, item_container, &policy);
}

/**
 * 	@warning: ONLY USE THIS IF item obeys the TypePool interface
 */
PUBLIC_API void *
AddToHashWithReference(HashTable *hash, void *item_value_ptr,
                       void (*    reference_incrementer_callback)(RecyclerClientData *, int))
{
  InsertPolicy policy = {
    .capacity    = INSERT_CAPACITY_GROW, .capacity_refusal_label = "NON RESIABLE TABLE REACHED CAPACITY",
    .on_accepted = reference_incrementer_callback
  };

  return _AddToHashInternal(hash, NULL, item_value_ptr, &policy);
}

/**
 * 	@brief The semantics of this routine allows the hashtable to recover from full capacity constraint by being told which item it
 * 	could evict from the hash in order to recover room.
 * 	If the item to be added is the same item to be evicted, nothing is done.
 * 	If item is evicted it is returned back to the caller.
 *
 * 	@param evicted_item_out: variable to save the evicted item into
 *
 * 	@warning FIX: item_evictor_callback is invoked while the write lock is held.
 * 	The callback MUST NOT re-enter this table (lock or mutate) or deadlock will occur.
 * 	Prefer a callback that only selects a victim pointer without locking the table.
 */
PUBLIC_API void *
AddToHashEvictIfNecessary(HashTable *hasht_ptr, const void *item_key, void *item_container_ptr,
                          void * (*item_evictor_callback)(ContextData *), ContextData *ctx_ptr, void **evicted_item_out)
{
  InsertPolicy policy = {
    .capacity    = INSERT_CAPACITY_EVICT, .evictor = item_evictor_callback, .evictor_ctx = ctx_ptr,
    .evicted_out = evicted_item_out
  };

  return _AddToHashInternal(hasht_ptr, item_key, item_container_ptr, &policy);
}

PUBLIC_API void *
AddToHashEvictIfNecessaryWithReference(HashTable *hasht_ptr, const void *item_key, void *item_container_ptr,
                                       void *(*   item_evictor_callback)(ContextData *), void **evicted_item_out,
                                       void (*    reference_incrementer_callback)(RecyclerClientData *, int),
                                       void (*    reference_decrementer_callback)(RecyclerClientData *, int))
{
  InsertPolicy policy = {
    .capacity    = INSERT_CAPACITY_EVICT, .evictor = item_evictor_callback, .evictor_ctx = NULL,
    .evicted_out = evicted_item_out, .on_accepted  = reference_incrementer_callback,
    .on_evicted  = reference_decrementer_callback
  };

  return _AddToHashInternal(hasht_ptr, item_key, item_container_ptr, &policy);
}

static ClientContextData *_DefaultItemExtractor(ItemContainer *item_container)
{
  return item_container;
}

/**
 *  @param item_key retrieved based on known offset in item container (eg. session_id)
 * 	@locked WR HashTable: by the caller
 * 	@return Container item in no extracted state
 */
static void *
_IsItemHashed(HashTable *hash, const void *item_key)
{
  unsigned int hashOffset;

  if (hash->fTableSize == 0) return NULL;

  hashOffset      = _ComputeHash(hash, item_key);
  void *value_ptr = NULL;

  void *start_p = hash->fTable[hashOffset]; /* container item, containing the key (eg Session) */

  if (IS_PRESENT(start_p) && IsItemEqualToKey(hash, HASHTABLE_EXTRACT_ITEM(hash, hash->fTable[hashOffset]), item_key)) {
    return start_p;
  }

  /* this separation is necessary to stop cyclic traversal of table where it is of fixed size, as capacity is allowed to reach zero, in which case there won't be null values to stop traversal */
  hashOffset = (hashOffset + 1) % hash->fTableSize;
  while (((value_ptr = hash->fTable[hashOffset]) != NULL) && !IsItemEqualToKey(
    hash, HASHTABLE_EXTRACT_ITEM(hash, value_ptr), item_key)) {
    if (hash->fTable[hashOffset] == start_p) {
      /* we did a full cycle and there was no match */
      LOAD_NULL(value_ptr);
      break;
    }

    hashOffset = (hashOffset + 1) % hash->fTableSize;
    value_ptr  = NULL;
  }

  if (IS_EMPTY(value_ptr)) {
#if UF_DEBUG_BUILD
    syslog(LOG_DEBUG, "%s (pid:'%lu'): %s: COULD NOT FIND HASHED ENTRY  using index offset:'%u'", __func__,
           pthread_self(), hash->table_name, hashOffset);
#endif
  }

  /* should be safe now as it returns a pointer to our own data anyway, unrelated to HashTable internal structure */
  return value_ptr;
}

/* readlock protected and safe */

/**
 *
 * @param hash Pre-allocated and initialised hastable object
 * @param item_key_ stored item which is used as key lookup
 * @param match_value currently always set to true
 * @return item container as originally stored by user in non-extracted state
 */
PUBLIC_API void *
HashLookup(HashTable *hash, void *item_key_, bool match_value)
{
  unsigned int hashOffset;

  if (unlikely(IS_EMPTY(hash))) return NULL;

  if (hash->flag_locking) if ((HashTable_RdLock(hash, 1)) != 0) return NULL;

  if (hash->fTableSize == 0) {
    if (hash->flag_locking) HashTable_UnLock(hash);

    return NULL;
  }

  //replace this block with _IsItemHashed()
  hashOffset      = _ComputeHash(hash, item_key_);
  void *value_ptr = NULL;

  if (match_value) {
    void *start_p = hash->fTable[hashOffset];

    if (IS_PRESENT(start_p) && IsItemEqualToKey(hash, HASHTABLE_EXTRACT_ITEM(hash, hash->fTable[hashOffset]),
                                                (const void*)item_key_)) {
      if (hash->flag_locking) HashTable_UnLock(hash);
      return start_p;
    }

    /* this separation is necessary to stop cyclic traversal of table where it is fixed size, as capacity is allowed to reach zero */
    hashOffset = (hashOffset + 1) % hash->fTableSize;
    while (((value_ptr = hash->fTable[hashOffset]) != NULL) && !IsItemEqualToKey(
      hash, HASHTABLE_EXTRACT_ITEM(hash, value_ptr), item_key_)) {
      if (hash->fTable[hashOffset] == start_p) {
        /* we did a full cycle and there was no match */
        LOAD_NULL(value_ptr);
        break;
      }

      hashOffset = (hashOffset + 1) % hash->fTableSize;
      value_ptr  = NULL;
    }
  }

  if (hash->flag_locking) HashTable_UnLock(hash);

  if (IS_EMPTY(value_ptr)) {
#if UF_DEBUG_BUILD
    syslog(LOG_DEBUG, "%s (pid:'%lu'): %s: COULD NOT FIND HASHED ENTRY  using index offset:'%u'", __func__,
           pthread_self(), hash->table_name, hashOffset);
#endif
  }

  /* should be safe now as it returns a pointer to our own data anyway, unrelated to HashTable internal structure */
  return value_ptr;
}

/**
 * 	@warning: ONLY USE THIS IF item obeys the TypePool interface
 */
PUBLIC_API void *
HashLookupWithReference(HashTable *hash, const void *item_key,
                        void (*    reference_incrementer_callback)(RecyclerClientData *, int))
{
  UfOptional *result = HashLookupWithReferenceOptional(hash, item_key, reference_incrementer_callback, NULL);
  if (!result) return NULL;
  void *holder = UfOptionalTake(result);
  UfOptionalDestroy(result);
  return holder;
}

PUBLIC_API UfOptional *
HashLookupWithReferenceOptional(HashTable *hash, const void *item_key,
                                void (*reference_incrementer_callback)(RecyclerClientData *, int), UfOptional *optional)
{
  if (!hash || !item_key || (optional && UfOptionalIsPresent(optional))) {
    errno = EINVAL;
    return NULL;
  }
  UfOptional *result = optional ? optional : UfOptionalEmpty(NULL, NULL);
  if (!result) {
    errno = ENOMEM;
    return NULL;
  }
  if (hash->flag_locking) {
    int lock_error = HashTable_RdLock(hash, 1);
    if (lock_error != 0) {
      if (!optional) UfOptionalDestroy(result);
      errno = lock_error;
      return NULL;
    }
  }
  void *holder = hash->fTableSize ? _IsItemHashed(hash, item_key) : NULL;
  if (holder) {
    if (reference_incrementer_callback) reference_incrementer_callback((RecyclerClientData*)holder, 1);
  }
  if (hash->flag_locking) HashTable_UnLock(hash);
  UfOptionalSet(result, holder, NULL, NULL);
  errno = 0;
  return result;
}

/* readlock UNSAFE */
/**
 *
 * @param hash
 * @param item_container Must be in unextracted state
 * @return
 */
PUBLIC_API void *
RemoveFromHash(HashTable *hash, void *item_container)
{
  return (_RemoveFromHash(hash, item_container, true, NULL));
}

PUBLIC_API void *
RemoveFromHashWithReference(HashTable *hash, void *item_container,
                            void (*    reference_decrementor_callback)(RecyclerClientData *, int))
{
  return (_RemoveFromHash(hash, item_container, true, reference_decrementor_callback));
}

/**
 * 	@locks WR Hashtable: if flagged
 * 	@unlocks 	Hashtable: if lock flagged
 * 	//readlock UNSAFE
 */
static void *
_RemoveFromHash(HashTable *hash, void *item_container, bool flag_lock_it,
                void (*    reference_decrementor_callback)(RecyclerClientData *, int))
{
  if (IS_EMPTY(hash) || IS_EMPTY(hash->fTable) || IS_EMPTY(item_container)) return NULL;
  /* hashtable not initialised, in use, or nothing to remove */

  if (flag_lock_it) if (hash->flag_locking) if ((HashTable_WrLock(hash, 0)) != 0) return NULL;

  unsigned int hashOffset;

  void *item_container_extracted = HASHTABLE_EXTRACT_ITEM(hash, item_container);
  if (IS_EMPTY(item_container_extracted)) return NULL;

  hashOffset = _ComputeHashFromItem(hash, item_container_extracted);

  /* A full fixed-size table has no NULL slot to stop the probe: bound it to one full cycle. */
  bool         found       = false;
  unsigned int probe_start = hashOffset;

  do {
    if (hash->fTable[hashOffset] == NULL) break;

    if (HASHTABLE_EXTRACT_ITEM(hash, hash->fTable[hashOffset]) == item_container_extracted) {
      found = true;
      break;
    }

    hashOffset = (hashOffset + 1) % hash->fTableSize;
  }
  while (hashOffset != probe_start);

  if (found) {
    hash->fTable[hashOffset] = NULL;
    hash->fNumEntries--;

    if (IS_PRESENT(reference_decrementor_callback))
      (*reference_decrementor_callback)((RecyclerClientData*)item_container, 1);

#ifdef __UF_FULLDEBUG
    syslog(LOG_DEBUG, "%s (pid='%lu'): %s: Hash index offset: '%u'. Current nEntries in HashTable: '%ld'", __func__,
           pthread_self(), hash->table_name, hashOffset, hash->fNumEntries);
#endif
  }

  /* compact to avoid fragmentation of buckets
   * FIX: use _ReinsertForCompact (no ExpandTable). Expanding mid-walk invalidated
   * indices and left compaction incomplete/incorrect. After one remove there is
   * always a free slot, so non-growing reinsert is safe. */
  hashOffset = (hashOffset + 1) % hash->fTableSize;

  unsigned     counter       = 0;
  /* Bound the walk to one full cycle to avoid unbounded loops on full fixed tables. */
  unsigned int start_compact = hashOffset;
  do {
    if (hash->fTable[hashOffset] == NULL) break;

    counter++;
    unsigned int origOffset = _ComputeHashFromItem(hash, HASHTABLE_EXTRACT_ITEM(hash, hash->fTable[hashOffset]));
    if (origOffset != hashOffset) {
      /* we need to shuffle this item */
      void *item = hash->fTable[hashOffset]; /* item to shuffle */
#if UF_DEBUG_BUILD
      syslog(LOG_DEBUG, "%s (pid:'%lu'): %s: COMPACTING ('%u'): using index offset: '%u'. nEntries in HashTable: '%ld'",
             __func__, pthread_self(), hash->table_name, counter, hashOffset, hash->fNumEntries);
#endif
      hash->fTable[hashOffset] = NULL;
      hash->fNumEntries--;

      _ReinsertForCompact(hash, item);
    }
    hashOffset = (hashOffset + 1) % hash->fTableSize;
  }
  while (hashOffset != start_compact);

  if (flag_lock_it) if (hash->flag_locking) HashTable_UnLock(hash);

  return found ? item_container : NULL;
}

PUBLIC_API long
MergeHashEntries(HashTable *destination, HashTable *source)
{
  if (unlikely(IS_EMPTY(destination) || IS_EMPTY(source))) return -1;

  void **snapshot = NULL;
  size_t occupied = 0;
  int    i;

  /* Snapshot the source, then release it: no lock may be held across the insertion loop,
   * otherwise destination locking nests inside source locking and opposite-direction
   * merges deadlock (and a self-merge upgrades its own read lock). */
  if (source->flag_locking) {
    if (HashTable_RdLock(source, 0) != 0) return -1;
  }

  if (IS_PRESENT(source->fTable)) {
    snapshot = (void**)calloc(source->fTableSize, sizeof(void*));
    if (IS_EMPTY(snapshot)) {
      if (source->flag_locking) HashTable_UnLock(source);
      return -1;
    }

    for (i = 0; i < (int)source->fTableSize; i++) {
      if (IS_PRESENT(source->fTable[i])) snapshot[occupied++] = source->fTable[i];
    }
  }

  if (source->flag_locking) HashTable_UnLock(source);

  /* A destination that cannot grow must take the whole snapshot or none of it: a partial
   * merge loses entries the caller believes were transferred. */
  if (!destination->flag_resizable) {
    size_t capacity = (destination->max_size > 0) ? destination->max_size : destination->fTableSize;

    if (capacity > 0) {
      size_t available = (destination->fNumEntries < (long)capacity) ? capacity - (size_t)destination->fNumEntries : 0;

      if (occupied > available) {
        syslog(
          LOG_ERR,
          "%s: %s: REFUSING MERGE: SOURCE ENTRIES ('%zu') EXCEED AVAILABLE CAPACITY ('%zu') OF NON-RESIZABLE DESTINATION: NO ENTRIES MERGED",
          __func__, destination->table_name, occupied, available);

        free(snapshot);

        return HASHTABLE_MERGE_REFUSED;
      }
    }
  }

  long merged = 0;

  for (i = 0; i < (int)occupied; i++) {
    if (IS_PRESENT(AddToHash(destination, snapshot[i]))) merged++;
  }

  free(snapshot);

  if (merged != (long)occupied) {
    syslog(LOG_ERR, "%s: %s: MERGE INCOMPLETE: SOURCE ENTRIES ('%zu') MERGED ('%ld')", __func__,
           destination->table_name, occupied, merged);
  }

  return merged;
}

/* Readlock SAFE */
int
GetHashEntries(HashTable *hash, void **itemArray, long itemArraySize)
{
  if (unlikely(IS_EMPTY(hash) || IS_EMPTY(hash->fTable))) return -1;

  if (hash->flag_locking) if ((HashTable_RdLock(hash, 1)) != 0) return -1;

  int i;
  int numResults = 0;

  for (i = 0; i < (int)hash->fTableSize; i++) {
    if (hash->fTable[i] != NULL) {
      itemArray[numResults++] = hash->fTable[i];
      if (numResults >= itemArraySize) break;
    }
  }

  if (hash->flag_locking) HashTable_UnLock(hash);

  return numResults;
}

/* Readlock SAFE */
PUBLIC_API BufferDescriptor *
DescribeHashTable(HashTable *hash, BufferDescriptor *provided)
{
  if (!provided) {
    provided = calloc(1, sizeof(BufferDescriptor));
    if (!provided) return NULL;

    BufferDescriptorInit(provided, 256);
    if (IS_EMPTY(provided->data)) {
      free(provided);
      return NULL;
    }
  }

  if (unlikely(IS_EMPTY(hash))) {
    BufferDescriptorAppendFormatted(provided, "{\"error\":\"null handle\"}\n");
    return provided;
  }

  const char *table_name = IS_EMPTY(hash->table_name) ? "AnonymousTable" : hash->table_name;

  /* Non-blocking by construction: a diagnostic probe must not queue behind a writer, and must
   * not deadlock when it runs on a thread that already owns the table. Reporting "busy" is
   * more useful than a stall, because the caller is an operator asking what the table looks like
   * right now, not a writer trying to make progress. */
  if (hash->flag_locking && HashTable_RdLock(hash, 1) != 0) {
    BufferDescriptorAppendFormatted(provided, "{\"table\":");
    JsonEscapeString(provided, table_name);
    BufferDescriptorAppendFormatted(provided, ",\"error\":\"busy\"}\n");
    return provided;
  }

  long   entries  = hash->fNumEntries;
  size_t buckets  = hash->fTableSize;
  size_t occupied = 0;

  for (size_t i = 0; i < buckets; i++) {
    if (hash->fTable[i] != NULL) occupied++;
  }

  if (hash->flag_locking) HashTable_UnLock(hash);

  double load_factor = (buckets > 0) ? (double)entries / (double)buckets : 0.0;

  /* key_size == 0 means a NUL-terminated string key (see _ComputeHash). */
  const char *key_kind = hash->fKeyIsPtr ? "ptr"
                       : (hash->fKeySize == KEY_SIZE_ZERO ? "string" : "inline");

  BufferDescriptorAppendFormatted(provided, "{\"table\":");
  JsonEscapeString(provided, table_name);
  BufferDescriptorAppendFormatted(provided,
    ",\"entries\":%ld,\"buckets\":%zu,\"occupied_buckets\":%zu,\"consistent\":%s"
    ",\"load_factor\":%.6g,\"max_size\":%zu,\"resizable\":%s,\"locking\":%s"
    ",\"unique_keys\":%s,\"item_printer\":%s"
    ",\"key\":{\"offset\":%ld,\"size\":%ld,\"kind\":\"%s\"}}\n",
    entries, buckets, occupied, (entries == (long)occupied) ? "true" : "false",
    load_factor,
    hash->max_size,
    hash->flag_resizable ? "true" : "false",
    hash->flag_locking ? "true" : "false",
    hash->flag_unique_keys ? "true" : "false",
    hash->item_pretty_printer_callback ? "true" : "false",
    hash->fKeyOffset, hash->fKeySize, key_kind);

  return provided;
}

int
HashTable_RdLock(HashTable *ht_ptr, int try_flag)
{
  int lock_state = 0;

  if (unlikely(IS_EMPTY(ht_ptr)) || !ht_ptr->flag_locking) return 0;

  if (try_flag) {
    /* The calling thread acquires the read lock if a writer does not hold the lock and there are no writers blocked on the lock. */
    lock_state = pthread_rwlock_tryrdlock(&(ht_ptr->hashtable_rwlock));
    if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
      syslog(LOG_DEBUG, "%s (pid='%lu' lock:80:1 ): SUCCESS: TRY-READ lock for HashTable '%s' acquired...", __func__,
             pthread_self(), ht_ptr->table_name);
#endif
    }
    else {
      char  error_str[MBUF] = {0};
      char *er              = strerror_r(errno, error_str, MBUF);

      syslog(
        LOG_NOTICE, "%s: {pid:'%lu', errno:'%d', error:'%s', e:'%d'): ERROR: COULD NOT ACQUIRE TRY-READ LOCK FOR '%s'",
        __func__, pthread_self(), errno, er, 815/*LOGCODE_RDLOCKTRY_FAIL*/, ht_ptr->table_name);
    }
  }
  else {
    lock_state = pthread_rwlock_rdlock(&(ht_ptr->hashtable_rwlock));

    if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
      syslog(LOG_DEBUG, "%s (pid='%lu' lock:80:1): %s: SUCCESSFULLY acquired READ lock", __func__, pthread_self(),
             ht_ptr->table_name);
#endif
    }
    else {
      char  error_str[MBUF] = {0};
      char *er              = strerror_r(errno, error_str, MBUF);

      syslog(LOG_NOTICE, "%s: {pid:'%lu', errno:'%d', error:'%s', e:'%d'}: ERROR: COULD NOT ACQUIRE READ LOCK FOR '%s'",
             __func__, pthread_self(), errno, er, 815/*LOGCODE_RDLOCKTRY_FAIL*/, ht_ptr->table_name);
    }
  }

  return lock_state;
}

int
HashTable_WrLock(HashTable *ht_ptr, int try_flag)
{
  int lock_state;

  if (unlikely(IS_EMPTY(ht_ptr)) || !ht_ptr->flag_locking) return 0;

  if (try_flag) {
    /* The calling thread acquires the write lock if a writer does not hold the lock and there are no writers blocked on the lock. */
    lock_state = pthread_rwlock_trywrlock(&(ht_ptr->hashtable_rwlock));
    if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
      syslog(LOG_DEBUG, "%s (pid='%lu' lock:80:1 ): SUCCESS: TRY-RW lock for HashTable '%s' acquired...", __func__,
             pthread_self(), ht_ptr->table_name);
#endif
    }
    else {
      char  error_str[MBUF] = {0};
      char *er              = strerror_r(errno, error_str, MBUF);

      syslog(LOG_NOTICE, "%s (pid='%lu'): ERROR: COULD NOT acquire TRY-RW lock for HashTable: '%s' (errno='%d'): '%s'",
             __func__, pthread_self(), ht_ptr->table_name, errno, er);
    }
  }
  else {
    lock_state = pthread_rwlock_wrlock(&(ht_ptr->hashtable_rwlock));

    if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
      syslog(LOG_DEBUG, "%s (pid='%lu'): %s: SUCCESSFULLY acquired RW lock...", __func__, pthread_self(),
             ht_ptr->table_name);
#endif
    }
    else {
      char  error_str[MBUF] = {0};
      char *er              = strerror_r(errno, error_str, MBUF);

      syslog(LOG_NOTICE, "%s (pid='%lu'): %s: COULD NOT acquire RW lock: error: '%s'", __func__, pthread_self(),
             ht_ptr->table_name, er);
    }
  }

  return lock_state;
}

int
HashTable_UnLock(HashTable *ht_ptr)
{
  if (unlikely(IS_EMPTY(ht_ptr)) || !ht_ptr->flag_locking) return 0;

  int lock_state = pthread_rwlock_unlock(&(ht_ptr->hashtable_rwlock));

  if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
    syslog(LOG_INFO, "%s (pid='%lu' lock:80:-1 ): %s: SUCCESSFULLY released lock...", __func__, pthread_self(),
           ht_ptr->table_name);
#endif
  }
  else {
    char  error_str[MBUF] = {0};
    char *er              = strerror_r(errno, error_str, MBUF);

    syslog(LOG_NOTICE, "%s (pid='%lu'): %s: COULD NOT acquire UNLOCK: error: '%s'", __func__, pthread_self(),
           ht_ptr->table_name, er);
  }

  return lock_state;
}
