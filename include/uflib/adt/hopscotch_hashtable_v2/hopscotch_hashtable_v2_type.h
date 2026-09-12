/**
 * @file
 * @brief Public types for the HopscotchHashTable V2 module.
 *
 * This header contains only types the consumer needs:
 *   - Opaque forward declaration of the handle type
 *   - Configuration struct
 *   - Callback typedefs (with Callback suffix)
 *
 * The implementation struct is defined in hopscotch_hashtable_v2_priv.h
 * (not installed).  sizeof(HopscotchHashTable) fails to compile in
 * consumer code, enforcing the opaque boundary.
 */

#ifndef UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_TYPE_H
#define UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_TYPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*! Opaque handle — definition in _priv.h, never visible to consumers. */
typedef struct HopscotchHashTable HopscotchHashTable;

/*!
 * @brief Callback to extract a lookup key from a stored data item.
 *
 * @param data_ptr  Pointer to the stored data item
 * @return Pointer to the key bytes within (or derived from) the data item
 *
 * NULL means the key starts at data_ptr + key_offset (default extraction).
 */
typedef const uint8_t *(*HopscotchHashTableKeyExtractorCallback)(
    const void *data_ptr);

/*!
 * @brief Callback to compute a custom hash from key bytes.
 *
 * @param key_ptr  Pointer to key bytes
 * @param key_len  Length of the key in bytes
 * @return 64-bit hash value
 *
 * NULL means use the default Jenkins hash function.
 */
typedef uint64_t (*HopscotchHashTableHashFunctionCallback)(
    const uint8_t *key_ptr, size_t key_len);

/*!
 * @brief Callback to compare two keys.
 *
 * @param a_ptr    Pointer to first key
 * @param b_ptr    Pointer to second key
 * @param key_len  Length of both keys in bytes
 * @return 0 if equal, non-zero if different
 *
 * NULL means use memcmp.
 */
typedef int (*HopscotchHashTableKeyCompareCallback)(
    const uint8_t *a_ptr, const uint8_t *b_ptr, size_t key_len);

/*!
 * @brief Iterator callback — invoked for each occupied entry.
 *
 * @param ctx_ptr   Opaque context passed through from ForEach
 * @param data_ptr  Pointer to the stored data item
 * @return true to continue iteration, false to stop early
 *
 * The callback receives caller-owned data items.  Returning false stops
 * iteration immediately; the ForEach function returns the count of
 * entries visited so far.
 */
typedef bool (*HopscotchHashTableForEachCallback)(
    void *ctx_ptr, void *data_ptr);

/*!
 * @brief Configuration for HopscotchHashTableCreate.
 *
 * All fields have sensible defaults when set to 0 / NULL / false.
 * Zero-initializing the struct ({0}) gives a working default config.
 */
typedef struct {
    /*! log2(initial bucket count).  0 -> CONFIG_DEFAULT_HOPSCOTCH_INIT_PFACTOR (6, 64 buckets). */
    size_t                                pfactor;

    /*! Neighbourhood size in slots.  0 -> CONFIG_DEFAULT_HOPSCOTCH_HOP_RANGE (32). */
    size_t                                hop_range;

    /*! Key width in bytes.  0 -> sizeof(uint64_t) (8 bytes). */
    size_t                                key_len;

    /*! Offset of the key within the data item.  0 -> key at data[0]. */
    size_t                                key_offset;

    /*! If true, duplicate keys are stored as separate entries (V1 behaviour).
     *  If false (default), inserting a duplicate key returns HOPSCOTCH_EDUPLICATE. */
    bool                                  allow_duplicates;

    /*! Custom key extractor.  NULL -> key at data_ptr + key_offset. */
    HopscotchHashTableKeyExtractorCallback   key_extractor_callback_ptr;

    /*! Custom hash function.  NULL -> default Jenkins hash. */
    HopscotchHashTableHashFunctionCallback   hash_func_callback_ptr;

    /*! Custom key comparator.  NULL -> memcmp. */
    HopscotchHashTableKeyCompareCallback     key_compare_callback_ptr;
} HopscotchHashTableConfig;

#endif /* UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_TYPE_H */
