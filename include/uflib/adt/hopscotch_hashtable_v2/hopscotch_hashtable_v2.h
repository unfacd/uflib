/**
 * @file
 * @brief Public API for the HopscotchHashTable V2 module.
 *
 * Hopscotch hashing (Herlihy, Shavit, and Tzafrir, DISC 2008) is an
 * open-addressing scheme where each bucket maintains a hopinfo bitmap
 * indicating which of the next H slots contain entries that hash to
 * this bucket.  This V2 module provides a single consolidated generic
 * variant with an opaque handle, PascalCase naming, and full Doxygen.
 *
 * This module is **single-threaded** — it provides no internal
 * synchronization.  Consumers needing concurrent access must provide
 * external synchronization (e.g., pthread_rwlock_t).
 *
 * V1 (adt_hopscotch_hashtable.h) is preserved in place alongside V2.
 * Consumers can use both modules simultaneously in the same process.
 *
 * @code{.c}
 * #include <uflib/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2.h>
 *
 * HopscotchHashTableConfig cfg = { .pfactor = 8 };
 * HopscotchHashTable *ht_ptr = HopscotchHashTableCreate(&cfg);
 * if (!ht_ptr) { // handle OOM }
 *
 * MyStruct item = { .id = 42, .name = "hello" };
 * HopscotchHashTableInsert(ht_ptr, &item);
 *
 * uint64_t key = 42;
 * MyStruct *found = HopscotchHashTableLookup(ht_ptr, (uint8_t *)&key);
 *
 * HopscotchHashTableDestroy(ht_ptr);
 * @endcode
 */

#ifndef UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_H
#define UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_H

#include <uflib/uflib_defs.h>
#include <uflib/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ────────────────────────────────────────────────────────── */

/**
 * @brief Create a new hopscotch hash table.
 *
 * Allocates the handle and the initial bucket array.  Config fields set to
 * 0 / NULL / false resolve to compile-time defaults from _defs.h.
 *
 * @param config_ptr  Configuration (NULL -> all defaults)
 * @return Opaque handle, or NULL on allocation failure
 *
 * @code{.c}
 * HopscotchHashTableConfig cfg = { .pfactor = 8, .key_offset = offsetof(MyStruct, id) };
 * HopscotchHashTable *ht_ptr = HopscotchHashTableCreate(&cfg);
 * if (!ht_ptr) { perror("create"); return -1; }
 * // ... use ht_ptr ...
 * HopscotchHashTableDestroy(ht_ptr);
 * @endcode
 */
PUBLIC_API HopscotchHashTable *
HopscotchHashTableCreate(const HopscotchHashTableConfig *config_ptr);

/**
 * @brief Destroy the hash table and free all internal memory.
 *
 * Does NOT free stored data items — those are caller-owned.  NULL-safe
 * (no-op on NULL).  After return the handle is invalid.
 *
 * @param ht_ptr  Table to destroy (may be NULL)
 *
 * @code{.c}
 * HopscotchHashTableDestroy(ht_ptr);
 * ht_ptr = NULL;  // good practice
 * @endcode
 */
PUBLIC_API void
HopscotchHashTableDestroy(HopscotchHashTable *ht_ptr);

/* ── Core Operations ──────────────────────────────────────────────────── */

/**
 * @brief Insert a data item into the table.
 *
 * The key is extracted from the data item using the config's extractor
 * callback (or data + key_offset if extractor is NULL).  If
 * allow_duplicates is false (default) and the key already exists,
 * returns HOPSCOTCH_EDUPLICATE.  The data item must remain valid and
 * at a stable address for the lifetime of the table entry.
 *
 * @param ht_ptr    Table handle
 * @param data_ptr  Data item to store (must not be NULL)
 * @return 0 on success, HOPSCOTCH_EDUPLICATE if key exists, -1 on table full
 *
 * @code{.c}
 * MyStruct item = { .id = 42, .name = "hello" };
 * int rc = HopscotchHashTableInsert(ht_ptr, &item);
 * if (rc == HOPSCOTCH_EDUPLICATE) { // duplicate key }
 * @endcode
 */
PUBLIC_API int
HopscotchHashTableInsert(HopscotchHashTable *ht_ptr, void *data_ptr);

/**
 * @brief Look up a data item by its key.
 *
 * @param ht_ptr   Table handle
 * @param key_ptr  Pointer to key bytes to search for
 * @return Data pointer if found, NULL if absent
 *
 * @code{.c}
 * uint64_t search_key = 42;
 * MyStruct *found = HopscotchHashTableLookup(ht_ptr, (uint8_t *)&search_key);
 * if (found) { printf("name: %s\n", found->name); }
 * @endcode
 */
PUBLIC_API void *
HopscotchHashTableLookup(HopscotchHashTable *ht_ptr, const uint8_t *key_ptr);

/**
 * @brief Remove and return a data item by key.
 *
 * @param ht_ptr   Table handle
 * @param key_ptr  Pointer to key bytes to remove
 * @return Removed data pointer, or NULL if not found
 *
 * @code{.c}
 * uint64_t key = 42;
 * MyStruct *removed = HopscotchHashTableRemove(ht_ptr, (uint8_t *)&key);
 * if (removed) { free(removed); }
 * @endcode
 */
PUBLIC_API void *
HopscotchHashTableRemove(HopscotchHashTable *ht_ptr, const uint8_t *key_ptr);

/* ── Query ────────────────────────────────────────────────────────────── */

/**
 * @brief Return the current number of stored entries.
 *
 * @param ht_ptr  Table handle
 * @return Entry count (0 for empty or NULL table)
 */
PUBLIC_API size_t
HopscotchHashTableSize(const HopscotchHashTable *ht_ptr);

/**
 * @brief Return the total bucket capacity.
 *
 * @param ht_ptr  Table handle
 * @return Total number of buckets (1 << pfactor), or 0 for NULL table
 */
PUBLIC_API size_t
HopscotchHashTableCapacity(const HopscotchHashTable *ht_ptr);

/**
 * @brief Return true if the table has no entries.
 *
 * @param ht_ptr  Table handle
 * @return true if entry_count == 0 or table is NULL
 */
PUBLIC_API bool
HopscotchHashTableIsEmpty(const HopscotchHashTable *ht_ptr);

/* ── Iteration ────────────────────────────────────────────────────────── */

/**
 * @brief Call callback for every occupied entry.
 *
 * Iteration order is **not** insertion order — it is bucket-array order.
 * If callback returns false, iteration stops early.
 *
 * @param ht_ptr          Table handle
 * @param callback_ptr    Called for each entry (return false to stop early)
 * @param ctx_ptr         Opaque context passed through to callback
 * @return Number of entries visited
 *
 * @code{.c}
 * static bool sPrintEntry(void *ctx_ptr, void *data_ptr) {
 *     MyStruct *s = (MyStruct *)data_ptr;
 *     printf("id=%lu\n", (unsigned long)s->id);
 *     return true;  // continue iteration
 * }
 * size_t n = HopscotchHashTableForEach(ht_ptr, sPrintEntry, NULL);
 * printf("visited %zu entries\n", n);
 * @endcode
 */
PUBLIC_API size_t
HopscotchHashTableForEach(HopscotchHashTable *ht_ptr,
                           HopscotchHashTableForEachCallback callback_ptr,
                           void *ctx_ptr);

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_H */
