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
#include <uflib/optional/ufoptional_type.h>
#include <uflib/recycler/recycler_type.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>

#define _CONFIGDEFAULT_HASHTABLE_EXPANSION_SZ							65521

#define KEY_SIZE_ZERO 0
/* Legacy names for the key-indirection flags; they describe the KEY, not the container.
 * Prefer HASHTABLE_KEY_IS_PTR / HASHTABLE_KEY_IS_INLINE in new code. */
#define HASH_ITEM_IS_PTR_TYPE 1
#define HASH_ITEM_NOT_PTR_TYPE 0
#define HASHTABLE_KEY_IS_PTR     HASH_ITEM_IS_PTR_TYPE
#define HASHTABLE_KEY_IS_INLINE  HASH_ITEM_NOT_PTR_TYPE
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
#define HASHTABLE_ISUNIQUEKEYS(x)	(x->flag_unique_keys)
#define HASHTABLE_DATA(x)	(x->fTable)

#define HASHTABLE_SETFLAG(x, y)	(x)->y=1
#define HASHTABLE_CLEARFLAG(x, y)	(x)->y=0

#define HASHTABLE_EXTRACT_ITEM(x, y) ((*x->item_extractor_callback)(y))
#define HASHTABLE_DEFAULT_EXTRACTOR NULL
#define HASHTABLE_DONT_INDICATE_IS_ADDED NULL

#define HASHTABLE_SELF_DESTRUCT true
#define HASHTABLE_ITEM_CONTAINER_OFFSET_ZERO 0
/* MergeHashEntries(): a non-resizable destination could not take the whole source
 * snapshot. Nothing was merged; destination and source are unchanged. */
#define HASHTABLE_MERGE_REFUSED (-2L)

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
 *
 * @brief Unique-key tables (opt-in, per table)
 *
 * HashTableSetUniqueKeys(hash, true) changes the insert contract: an insert whose
 * key already exists does NOT add a second entry.  The insert returns the container
 * that holds the key afterwards — the stored one — and reports is_added = true.
 * The caller therefore distinguishes "I claimed the key" (returned == passed) from
 * "someone else already holds it" (returned != passed).
 *
 * Uniqueness is enforced inside the table's write lock, so an insert is an atomic
 * claim and no external locking is needed.  Default is off: inserts compare
 * extracted-item pointers and duplicate keys are stored as separate entries.
 */

/* Key configuration, fixed at instantiate time. Three independent dimensions:
 *
 *   offset       offsetof(Item, key_field), resolved within the EXTRACTED item.
 *   key_is_ptr   HASHTABLE_KEY_IS_PTR     the field is a pointer to the key:  char *name
 *                HASHTABLE_KEY_IS_INLINE  the key is stored inline:           char name[N]
 *                                                                           unsigned long id
 *   extractor    container -> item. Pass a container to the API and the table calls the
 *                extractor before touching the key; HASHTABLE_DEFAULT_EXTRACTOR means
 *                container == item.
 *
 * Pitfalls:
 *   - key_is_ptr switches key indirection only; it does not say whether callers hold
 *     containers. A pointer-keyed table with a container extractor sets both.
 *   - Give the API exactly one level: containers to a table with an extractor, extracted
 *     items to one without. The wrong level makes every key read address the wrong object.
 *   - A unique-key table compares key values, so a resident item's key must not be mutated.
 */
/**
 * @brief Create a table over caller-owned storage (no lock).
 * @param[in,out] ht_ptr Caller storage, or NULL to allocate one.
 * @param[in] offset offsetof(Item, key_field) within the extracted item.
 * @param[in] key_size sizeof(key) for a fixed-width key, KEY_SIZE_ZERO for a string.
 * @param[in] key_is_ptr HASHTABLE_KEY_IS_PTR if the field points at the key, HASHTABLE_KEY_IS_INLINE if inline.
 * @param[in] name Table name for diagnostics, or NULL for "AnonymousTable".
 * @param[in] item_extractor_callback Container-to-item mapping, or HASHTABLE_DEFAULT_EXTRACTOR.
 * @return Initialised table, or NULL on allocation failure.
 * @note Without a lock the caller serialises access; see HashTableLockingInstantiate().
 * @note The returned table owns its bucket array; items stay caller-owned.
 */
HashTable *HashTableInstantiate(HashTable *, int offset, int key_size, long key_is_ptr, const char *,
                                ItemExtractor item_extractor_callback);
/**
 * @brief Create a table that takes its own read/write lock around every operation.
 * @param[in,out] ht_ptr Caller storage, or NULL to allocate one.
 * @param[in] offset offsetof(Item, key_field) within the extracted item.
 * @param[in] key_size sizeof(key) for a fixed-width key, KEY_SIZE_ZERO for a string.
 * @param[in] key_is_ptr HASHTABLE_KEY_IS_PTR if the field points at the key, HASHTABLE_KEY_IS_INLINE if inline.
 * @param[in] name Table name for diagnostics, or NULL for "AnonymousTable".
 * @param[in] item_extractor_callback Container-to-item mapping, or HASHTABLE_DEFAULT_EXTRACTOR.
 * @return Initialised table, or NULL on allocation or rwlock-initialisation failure.
 * @note Re-instantiating caller storage destroys its previous lock and bucket array.
 */
PUBLIC_API HashTable *HashTableLockingInstantiate(HashTable *ht_ptr_in, int offset, int key_size, long key_is_ptr,
                                                  const char *, ItemExtractor item_extractor_callback);
/**
 * @brief Instantiate a table from a HashtableSpecs descriptor (no lock).
 * @param[in,out] ht_ptr Caller storage, or NULL to allocate one.
 * @param[in] specs Descriptor providing the key, name, extractor, and optional startup bucket count.
 * @return Initialised table, or NULL for invalid specs or allocation failure.
 * @note Buckets are allocated at first insertion. fTableSize of zero uses the
 *       default count, and one is rounded to two. The table remains resizable.
 */
PUBLIC_API HashTable *HashTableInstantiateWithSpecs(HashTable *ht_ptr, const HashtableSpecs *specs);
/**
 * @brief Instantiate a locking table from a HashtableSpecs descriptor.
 * @param[in,out] ht_ptr Caller storage, or NULL to allocate one.
 * @param[in] specs Descriptor providing the key, name, extractor, and optional startup bucket count.
 * @return Initialised table, or NULL for invalid specs or allocation failure.
 * @note Buckets are allocated at first insertion, using the same count rules
 *       as HashTableInstantiateWithSpecs().
 */
PUBLIC_API HashTable *HashTableLockingInstantiateWithSpecs(HashTable *ht_ptr, const HashtableSpecs *specs);
/**
 * @brief Opt this table into key-value uniqueness. Fixed for the table's life.
 * @param[in,out] hash Initialised table.
 * @param[in] unique_keys true enforces uniqueness, false restores the default.
 * @return \p hash, or NULL when \p hash is NULL.
 * @note Must be called before the table takes concurrent traffic; the flag is not atomic.
 * @note Inserts compare key values, so a resident item's key must not be mutated.
 */
PUBLIC_API HashTable *HashTableSetUniqueKeys(HashTable *hash, bool unique_keys);
/**
 * @brief Release the table's bucket array, name and lock. Items stay caller-owned.
 * @param[in,out] hash Table to tear down, or NULL.
 * @param[in] is_self_destruct Free \p hash itself as well (true when it was allocated internally).
 * @note Undefined once the table has been used after destruction; no operation on \p hash is safe afterwards.
 */
PUBLIC_API void       HashTableDestruct(HashTable *hash, bool is_self_destruct);

/**
 * @brief Insert a container without reporting the outcome.
 * @param[in,out] hash Initialised table.
 * @param[in] item_container Container to insert; extracted through the table's extractor.
 * @return Container resident for the key: \p item_container on insertion, or the incumbent when
 *         a unique-key table already holds the key (the refused container is not stored).
 * @note Default (non-unique) tables store duplicate keys as separate entries.
 * @code{.c}
 * HashTable *hash = HashTableLockingInstantiate(NULL, offsetof(Session, session_id),
 *                                               sizeof(unsigned long), HASHTABLE_KEY_IS_INLINE,
 *                                               "SessionsHashTable", HASHTABLE_DEFAULT_EXTRACTOR);
 * if (AddToHash(hash, instance_sesn_ptr) != instance_sesn_ptr) {  // a resident already held the key
 *     // handle the refusal
 * }
 * @endcode
 */
PUBLIC_API void *AddToHash(HashTable *hash, void *item);
/**
 * @brief Insert a container and report whether the key was already resident.
 * @param[in,out] hash Initialised table.
 * @param[in] item_container Container to insert.
 * @param[out] is_added Optional. Set to true when an equal key was already resident, false when
 *             this call inserted. The name is historical: true means "already hashed".
 * @return Container resident for the key; see AddToHash().
 */
PUBLIC_API void *AddToHashWithIndication(HashTable *hash, void *item_container, bool *is_added);
/**
 * @brief Insert into a unique-key table, reporting who holds the key afterwards.
 * @param[in,out] hash Table configured with HashTableSetUniqueKeys(hash, true).
 * @param[in] item_container Container to insert.
 * @param[out] was_inserted Optional. True only when \p item_container itself became resident.
 * @param[out] resident_container Optional. The container holding the key afterwards: \p item_container
 *             on a claim, the incumbent on a refusal, NULL when nothing was attempted.
 * @return Resident container, or NULL when the table is not unique-keyed or the insert failed.
 * @note A refusal is not an error; compare the returned pointer with the one offered.
 * @code{.c}
 * bool inserted = false;
 * void *resident = NULL;
 * AddToHashUniquely(hash, instance_f_ptr, &inserted, &resident);
 * if (!inserted && resident != instance_f_ptr) {
 *     // another instance claimed the key; this one was not stored
 * }
 * @endcode
 */
PUBLIC_API void *AddToHashUniquely(HashTable *hash, void *item_container, bool *was_inserted,
                                   void **    resident_container);
/**
 * @brief Insert into a unique-key table, displacing whatever instance holds the key.
 * @param[in,out] hash Table configured with HashTableSetUniqueKeys(hash, true).
 * @param[in] item_container Container to insert.
 * @param[out] replaced_container Optional. The displaced incumbent, or NULL when the key was free
 *             or already held by \p item_container.
 * @return Resident container, or NULL when the table is not unique-keyed or the insert failed.
 * @note Use where authority outside the table decides which instance must hold the key; the
 *       incumbent is treated as stale rather than authoritative.
 */
/* Displace whichever instance holds the key: for callers whose authority sits outside the table. */
PUBLIC_API void *AddToHashReplacing(HashTable *hash, void *item_container, void **replaced_container);
/**
 * @brief Insert a container, retaining it through a callback when it becomes resident.
 * @param[in,out] hash Initialised table.
 * @param[in] item_value_ptr Container to insert.
 * @param[in] reference_incrementer_callback Retain callback, invoked under the write lock on acceptance.
 * @return Container resident for the key; see AddToHash().
 * @warning The callback runs under the write lock and must not re-enter this table.
 * @warning Only use with items that obey the RecyclerClientData interface.
 */
PUBLIC_API void *AddToHashWithReference(HashTable *hash, void *item,
                                        void (*    reference_incrementer_callback)(RecyclerClientData *, int));
/**
 * @brief Insert, recovering a full table by evicting a caller-nominated victim.
 * @param[in,out] hasht_ptr Initialised table.
 * @param[in] item_key Lookup key for the item being inserted, or NULL to skip pre-checks.
 * @param[in] item_container_ptr Container to insert.
 * @param[in] item_evictor_callback Selects a victim from \p ctx_ptr; runs under the write lock.
 * @param[in] ctx_ptr Opaque argument for \p item_evictor_callback.
 * @param[out] evicted_item_out Optional. The evicted container, or NULL when nothing was evicted.
 * @return Resident container, or NULL when the insert could not be performed.
 * @warning The evictor runs under the write lock and must not re-enter this table.
 */
PUBLIC_API void *AddToHashEvictIfNecessary(HashTable *hasht_ptr, const void *item_key, void *item_container_ptr,
                                           evictor_callback, ContextData *, void **);
/**
 * @brief As AddToHashEvictIfNecessary(), with retain on acceptance and release on eviction.
 * @param[in,out] hasht_ptr Initialised table.
 * @param[in] item_key Lookup key for the item being inserted, or NULL to skip pre-checks.
 * @param[in] item_container_ptr Container to insert.
 * @param[in] item_evictor_callback Selects a victim; runs under the write lock.
 * @param[out] evicted_item_out Optional. The evicted container.
 * @param[in] reference_incrementer_callback Retain callback for the accepted container.
 * @param[in] reference_decrementer_callback Release callback for the evicted container.
 * @return Resident container, or NULL when the insert could not be performed.
 * @warning Both callbacks run under the write lock and must not re-enter this table.
 */
PUBLIC_API void *AddToHashEvictIfNecessaryWithReference(HashTable *hasht_ptr, const void *item_key,
                                                        void *     item_container_ptr,
                                                        void *(*   item_evictor_callback)(ContextData *), void **,
                                                        void (*    reference_incrementer_callback)(
                                                          RecyclerClientData *, int),
                                                        void (*reference_decrementer_callback)(
                                                          RecyclerClientData *, int));
/**
 * @brief Look up the container holding a key.
 * @param[in,out] hash Initialised table.
 * @param[in] item_key Key in the table's configured format (pointer or inline value, per key_is_ptr).
 * @param[in] match_value Must be true; retained for historical compatibility.
 * @return Stored container, or NULL on absence or failure. Locking tables use a try-read-lock.
 * @note Returns the container as supplied on insert, not the extracted item.
 * @code{.c}
 * void *holder = HashLookup(hash, &session_id, true);
 * if (holder != NULL) {
 *     Session *sesn_ptr = SessionOffInstanceHolder(holder);
 * }
 * @endcode
 */
PUBLIC_API void *HashLookup(HashTable *hash, void *data, bool);
/**
 * @brief Compatibility lookup returning the referenced holder, or NULL for absence/error.
 * @param[in,out] hash Initialized table.
 * @param[in] item_key Lookup key in the configured format.
 * @param[in] reference_incrementer_callback Retain callback, or NULL for a borrowed value.
 * @return Found container; NULL on absence or operational failure.
 * @note Delegates to HashLookupWithReferenceOptional(), takes its payload and destroys the wrapper.
 *       Any acquired reference remains caller-owned. This path now allocates an optional wrapper.
 * @code{.c}
 * void *holder = HashLookupWithReference(hash, key, RetainHolder);
 * if (holder != NULL) {
 *     ReleaseHolder((RecyclerClientData *)holder, 1);
 * }
 * @endcode
 */
PUBLIC_API void *HashLookupWithReference(HashTable *hash, const void *item_key,
                                         void (*reference_incrementer_callback)(RecyclerClientData *, int));
/**
 * @brief Look up an optional holder, distinguishing operational failure from absence.
 * @param[in,out] hash Initialized table; locking mode uses a try-read-lock.
 * @param[in] item_key Non-NULL lookup key in the table's configured key format.
 * @param[in] reference_incrementer_callback Retains a found container under the lookup lock;
 *            NULL preserves borrowed-value lookup semantics.
 * @param[in,out] optional Empty reusable optional, or NULL to allocate a new wrapper.
 * @return Supplied/new present or empty optional on successful lookup; NULL on error.
 * @note On error errno is EINVAL for NULL hash/key or a nonempty supplied optional, ENOMEM for optional allocation failure,
 *       or the actual pthread error returned by hash-lock acquisition, including EBUSY.
 *       Successful present and absent lookups set errno to zero.
 * @note The optional has no payload destructor. The caller owns any acquired reference:
 *       UfOptionalDestroy() releases only the wrapper, and UfOptionalTake() transfers the pointer.
 *       Supplied optionals remain caller-owned and unchanged on failure.
 *       Allocate/lookup failures acquire no reference. Caller synchronization is required for non-locking tables.
 * @warning The retain callback runs inside the hash read lock; it must not mutate/reenter this table
 *          or acquire locks in conflict with table writers. Hash/key/table lifetime remains caller-owned.
 * @code{.c}
 * UfOptional *result = HashLookupWithReferenceOptional(hash, key, RetainHolder, NULL);
 * if (result == NULL) {
 *     return errno;
 * }
 * void *holder = UfOptionalTake(result);
 * UfOptionalDestroy(result);
 * if (holder != NULL) {
 *     ReleaseHolder((RecyclerClientData *)holder, 1);
 * }
 * @endcode
 */
PUBLIC_API UfOptional *HashLookupWithReferenceOptional(HashTable *hash, const void *item_key,
                                                       void (*reference_incrementer_callback)(
                                                         RecyclerClientData *, int), UfOptional *optional);
/**
 * @brief Remove a container's entry from the table.
 * @param[in,out] hash Initialised table.
 * @param[in] item_container Container previously inserted.
 * @return The removed container, or NULL when it was not resident.
 * @note Removal compacts the probe sequence; other containers' positions may change.
 */
PUBLIC_API void *RemoveFromHash(HashTable *hash, void *item_container);
/**
 * @brief Remove an entry, releasing a held reference through a callback.
 * @param[in,out] hash Initialised table.
 * @param[in] item_container Container previously inserted.
 * @param[in] reference_decrementer_callback Release callback, invoked under the write lock on removal.
 * @return The removed container, or NULL when it was not resident.
 * @warning The callback runs under the write lock and must not re-enter this table.
 */
PUBLIC_API void *RemoveFromHashWithReference(HashTable *hash, void *item_value_ptr,
                                             void (*    reference_decrementor_callback)(RecyclerClientData *, int));

/**
 * @brief Move every source entry into the destination.
 * @param[in,out] destination Target table; locking tables are written entry by entry.
 * @param[in,out] source Table snapshotted under its read lock, then released before merging.
 * @return Number of containers merged (0 for an empty source), HASHTABLE_MERGE_REFUSED when a
 *         non-resizable destination lacks capacity and nothing was merged, or -1 on invalid
 *         arguments or snapshot allocation failure.
 * @note A non-resizable destination takes the whole snapshot or none of it.
 * @note Entries are not removed from \p source; destroy it separately if it is now redundant.
 */
PUBLIC_API long MergeHashEntries(HashTable *destination, HashTable *source);
/**
 * @brief Copy resident containers into a caller array.
 * @param[in,out] hash Initialised table; locking tables take a try-read-lock.
 * @param[out] itemArray Caller array of at least \p itemArraySize elements.
 * @param[in] itemArraySize Capacity of \p itemArray.
 * @return Number of containers written, or -1 on invalid arguments or lock failure.
 * @note Snapshot only: entries added or removed afterwards are not reflected.
 */
PUBLIC_API int  GetHashEntries(HashTable *hash, void **itemArray, long itemArraySize);
/**
 * @brief Summarise a table's configuration and occupancy as a JSON object.
 * @param[in,out] hash Initialised table; a locking table is read under a try-read lock.
 * @param[out] provided Caller storage for the descriptor, or NULL to allocate one.
 * @return \p provided holding exactly one JSON object (newline-terminated), or NULL when
 *         \p provided was NULL and the descriptor could not be allocated.
 * @note Never blocks. A locking table whose write lock is held (or is writer-contended)
 *       reports {"table":...,"error":"busy"}.
 * @note Item payloads are not walked: the item printer callback is diagnostic-only and
 *       returns an unspecified-ownership pointer.
 * @note `consistent` is false when fNumEntries disagrees with the number of occupied
 *       buckets; that means either a concurrent mutation the try-read lock did not
 *       exclude, or bookkeeping drift worth investigating.
 * @note The caller owns the buffer: BufferDescriptorRelease() it, including when \p
 *       provided was NULL and this function allocated it.
 */
PUBLIC_API BufferDescriptor *DescribeHashTable(HashTable *hash, BufferDescriptor *provided);


/**
 * @brief Acquire the table's read lock.
 * @param[in,out] ht_ptr Table; a non-locking table is a no-op returning 0.
 * @param[in] try_flag Non-zero for pthread_rwlock_tryrdlock, 0 for a blocking acquire.
 * @return 0 on success or no-op, otherwise the pthread error (EBUSY for a failed try).
 * @note Rarely needed directly: the locked-table verbs already take the lock themselves.
 */
PUBLIC_API int HashTable_RdLock(HashTable *ht_ptr, int);
/**
 * @brief Acquire the table's write lock.
 * @param[in,out] ht_ptr Table; a non-locking table is a no-op returning 0.
 * @param[in] try_flag Non-zero for pthread_rwlock_trywrlock, 0 for a blocking acquire.
 * @return 0 on success or no-op, otherwise the pthread error (EBUSY for a failed try).
 * @note External write locking is only needed to make several operations atomic together.
 */
PUBLIC_API int HashTable_WrLock(HashTable *ht_ptr, int);
/**
 * @brief Release a lock taken by HashTable_RdLock() or HashTable_WrLock().
 * @param[in,out] ht_ptr Table; a non-locking table is a no-op returning 0.
 * @return 0 on success or no-op, otherwise the pthread error.
 */
PUBLIC_API int HashTable_UnLock(HashTable *ht_ptr);

#endif /* UFLIB_ADT_ADT_HASHTABLE_H */
