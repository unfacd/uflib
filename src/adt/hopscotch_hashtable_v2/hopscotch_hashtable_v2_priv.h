/**
 * @file
 * @brief Private types for the HopscotchHashTable V2 module.
 *
 * NOT INSTALLED.  This header is colocated with the implementation (.c) and
 * is visible only to the uflib build and test files.  Consumers never see
 * these definitions — they work with the opaque HopscotchHashTable pointer.
 */

#ifndef UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_PRIV_H
#define UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_PRIV_H

#include <uflib/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2_type.h>
#include <uflib/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2_defs.h>

/*! Internal bucket — one per slot in the bucket array. */
typedef struct {
    void     *data_ptr;     /*!< Caller-owned data item.  NULL = empty slot.   */
    uint32_t  hopinfo;      /*!< 32-bit bitmap — bit i set means buckets[idx+i]
                                 belongs to this bucket's neighbourhood.       */
} HopscotchBucket;

/*!
 * @brief Opaque handle struct — defined only here and in the .c file.
 *
 * Consumers receive a pointer to this struct but never see its definition.
 * sizeof(HopscotchHashTable) fails to compile in consumer code.
 */
struct HopscotchHashTable {
    size_t                    pfactor;       /*!< log2(bucket count). Changes on resize.    */
    HopscotchBucket          *buckets_ptr;   /*!< Bucket array, size = 1 << pfactor.       */
    size_t                    entry_count;   /*!< Current number of stored entries.         */
    HopscotchHashTableConfig  config;        /*!< Frozen copy of creation config.           */
};

/* ── Error codes ─────────────────────────────────────────────────────── */

/*! Insert rejected: duplicate key with allow_duplicates=false. */
#define HOPSCOTCH_EDUPLICATE  (-2)

#endif /* UFLIB_ADT_HOPSCOTCH_HASHTABLE_V2_PRIV_H */
