/**
 * @file
 * @brief Implementation of the HopscotchHashTable V2 module.
 *
 * Single consolidated generic variant — replaces the three parallel V1
 * variants (offset-based, keyed, configurable) with one implementation
 * parameterised by the config struct.
 *
 * Concurrency: single-threaded.  No internal synchronization.
 * Consumers needing concurrent access must provide external locking.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2.h>
#include "hopscotch_hashtable_v2_priv.h"

#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <pthread.h>

/* ── Static helpers (s prefix) ──────────────────────────────────────── */

/*
 * Jenkins hash function (one-at-a-time).
 * Deterministic — same key produces same 32-bit hash.
 */
static uint32_t
sJenkinsHash(const uint8_t *key_ptr, size_t len)
{
    uint32_t hash = 0;
    for (size_t i = 0; i < len; i++) {
        hash += key_ptr[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

/*
 * Compute the hash index for a key.  Uses the config's custom hash
 * function if provided, otherwise falls back to Jenkins.
 */
static size_t
sHashToIndex(const HopscotchHashTable *ht_ptr, const uint8_t *key_ptr)
{
    uint64_t h;
    if (ht_ptr->config.hash_func_callback_ptr) {
        h = ht_ptr->config.hash_func_callback_ptr(
            key_ptr, ht_ptr->config.key_len);
    } else {
        h = sJenkinsHash(key_ptr, ht_ptr->config.key_len);
    }
    return (size_t)(h & ((1UL << ht_ptr->pfactor) - 1));
}

/*
 * Extract the key from a data item.  Uses the config's extractor callback
 * if provided, otherwise key is at data_ptr + key_offset.
 */
static const uint8_t *
sExtractKey(const HopscotchHashTable *ht_ptr, const void *data_ptr)
{
    if (ht_ptr->config.key_extractor_callback_ptr) {
        return ht_ptr->config.key_extractor_callback_ptr(data_ptr)
               + ht_ptr->config.key_offset;
    }
    return (const uint8_t *)data_ptr + ht_ptr->config.key_offset;
}

/*
 * Compare two keys.  Uses the config's comparator if provided,
 * otherwise falls back to memcmp.
 */
static int
sKeyCompare(const HopscotchHashTable *ht_ptr,
            const uint8_t *a_ptr, const uint8_t *b_ptr)
{
    if (ht_ptr->config.key_compare_callback_ptr) {
        return ht_ptr->config.key_compare_callback_ptr(
            a_ptr, b_ptr, ht_ptr->config.key_len);
    }
    return memcmp(a_ptr, b_ptr, ht_ptr->config.key_len);
}

/*
 * Scan the neighbourhood of bucket[idx] for an entry matching key_ptr.
 * Returns the data pointer if found, NULL otherwise.
 */
static void *
sFindInNeighbourhood(HopscotchHashTable *ht_ptr, size_t idx,
                     const uint8_t *key_ptr)
{
    size_t hop_range = ht_ptr->config.hop_range;
    HopscotchBucket *buckets = ht_ptr->buckets_ptr;

    if (!buckets[idx].hopinfo) {
        return NULL;
    }

    for (size_t i = 0; i < hop_range; i++) {
        if (buckets[idx].hopinfo & (1UL << i)) {
            void *candidate = buckets[idx + i].data_ptr;
            if (candidate) {
                const uint8_t *candidate_key = sExtractKey(ht_ptr, candidate);
                if (0 == sKeyCompare(ht_ptr, key_ptr, candidate_key)) {
                    return candidate;
                }
            }
        }
    }
    return NULL;
}

/*
 * Displace an entry forward to make room for a new insert.
 * Scans backwards from slot 'empty_idx' looking for the closest bucket
 * with a non-zero hopinfo entry that can be displaced forward.
 *
 * Returns true if a displacement was performed, false if none possible.
 */
static bool
sDisplaceForward(HopscotchHashTable *ht_ptr, size_t bucket_idx,
                 size_t *empty_idx_ptr)
{
    size_t hop_range = ht_ptr->config.hop_range;
    HopscotchBucket *buckets = ht_ptr->buckets_ptr;
    size_t i = *empty_idx_ptr;

    for (size_t j = 1; j < hop_range; j++) {
        if (i < j) break;  /* can't scan before index 0 */
        if (buckets[i - j].hopinfo) {
            size_t off = (size_t)__builtin_ctz(buckets[i - j].hopinfo);
            if (off >= j) continue;  /* can't displace backwards */

            /* Displace: move entry from [i-j+off] to [i] */
            buckets[i].data_ptr = buckets[i - j + off].data_ptr;
            buckets[i - j + off].data_ptr = NULL;
            buckets[i - j].hopinfo &= ~(1UL << off);
            buckets[i - j].hopinfo |= (1UL << j);
            *empty_idx_ptr = i - j + off;
            return true;
        }
    }
    return false;
}

/*
 * Resize the table to 2x capacity, re-inserting all entries.
 * Uses the config's hash function consistently — resolves D-1 (HOP-026).
 *
 * Returns 0 on success, -1 on allocation failure (old table restored).
 */
static int
sResize(HopscotchHashTable *ht_ptr)
{
    size_t old_capacity = 1UL << ht_ptr->pfactor;
    size_t new_pfactor = ht_ptr->pfactor + PRIV_CONFIG_DEFAULT_HOPSCOTCH_RESIZE_DELTA;
    size_t new_capacity = 1UL << new_pfactor;

    syslog(LOG_DEBUG,
        "%s {pid:'%lu', old_cap:'%zu', new_cap:'%zu'}: Resizing table...",
        __func__, (unsigned long)pthread_self(), old_capacity, new_capacity);

    HopscotchBucket *new_buckets = calloc(new_capacity, sizeof(HopscotchBucket));
    if (!new_buckets) {
        return -1;
    }

    /* Swap in the new array */
    HopscotchBucket *old_buckets = ht_ptr->buckets_ptr;
    size_t old_pfactor = ht_ptr->pfactor;

    ht_ptr->buckets_ptr = new_buckets;
    ht_ptr->pfactor = new_pfactor;

    /* Re-insert all entries from the old array using the SAME hash function */
    for (size_t i = 0; i < old_capacity; i++) {
        if (old_buckets[i].data_ptr) {
            void *data_ptr = old_buckets[i].data_ptr;
            const uint8_t *key_ptr = sExtractKey(ht_ptr, data_ptr);
            size_t idx = sHashToIndex(ht_ptr, key_ptr);
            bool inserted = false;

            /* Linear probe in the new (larger) array */
            for (size_t j = idx; j < new_capacity; j++) {
                if (!new_buckets[j].data_ptr) {
                    size_t off = j - idx;
                    if (off >= ht_ptr->config.hop_range) {
                        /* Shouldn't happen on first insert after resize —
                         * the table just doubled.  Fall through to error. */
                        break;
                    }
                    new_buckets[j].data_ptr = data_ptr;
                    new_buckets[idx].hopinfo |= (1UL << off);
                    inserted = true;
                    break;
                }
            }

            if (!inserted) {
                /* Restore old state on failure */
                ht_ptr->buckets_ptr = old_buckets;
                ht_ptr->pfactor = old_pfactor;
                free(new_buckets);
                return -1;
            }
        }
    }

    free(old_buckets);
    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

PUBLIC_API HopscotchHashTable *
HopscotchHashTableCreate(const HopscotchHashTableConfig *config_ptr)
{
    static const HopscotchHashTableConfig sDefaultConfig = {0};
    const HopscotchHashTableConfig *cfg = config_ptr ? config_ptr : &sDefaultConfig;

    HopscotchHashTable *ht_ptr = calloc(1, sizeof(HopscotchHashTable));
    if (!ht_ptr) {
        return NULL;
    }

    /* Resolve defaults for zero fields */
    size_t pfactor = cfg->pfactor ? cfg->pfactor
                                  : CONFIG_DEFAULT_HOPSCOTCH_INIT_PFACTOR;
    size_t hop_range = cfg->hop_range ? cfg->hop_range
                                       : CONFIG_DEFAULT_HOPSCOTCH_HOP_RANGE;
    size_t key_len = cfg->key_len ? cfg->key_len
                                  : CONFIG_DEFAULT_HOPSCOTCH_KEY_WIDTH;

    size_t capacity = 1UL << pfactor;
    ht_ptr->buckets_ptr = calloc(capacity, sizeof(HopscotchBucket));
    if (!ht_ptr->buckets_ptr) {
        free(ht_ptr);
        return NULL;
    }

    ht_ptr->pfactor = pfactor;
    ht_ptr->entry_count = 0;

    /* Store a frozen copy of the config */
    ht_ptr->config.pfactor = pfactor;
    ht_ptr->config.hop_range = hop_range;
    ht_ptr->config.key_len = key_len;
    ht_ptr->config.key_offset = cfg->key_offset;
    ht_ptr->config.allow_duplicates = cfg->allow_duplicates;
    ht_ptr->config.key_extractor_callback_ptr = cfg->key_extractor_callback_ptr;
    ht_ptr->config.hash_func_callback_ptr = cfg->hash_func_callback_ptr;
    ht_ptr->config.key_compare_callback_ptr = cfg->key_compare_callback_ptr;

    return ht_ptr;
}

PUBLIC_API void
HopscotchHashTableDestroy(HopscotchHashTable *ht_ptr)
{
    if (!ht_ptr) {
        return;  /* NULL-safe */
    }
    free(ht_ptr->buckets_ptr);
    ht_ptr->buckets_ptr = NULL;
    free(ht_ptr);
}

/* ── Core Operations ──────────────────────────────────────────────────── */

PUBLIC_API int
HopscotchHashTableInsert(HopscotchHashTable *ht_ptr, void *data_ptr)
{
    if (!ht_ptr || !data_ptr) {
        return -2;  /* NULL data rejected */
    }

    const uint8_t *key_ptr = sExtractKey(ht_ptr, data_ptr);
    size_t idx = sHashToIndex(ht_ptr, key_ptr);
    size_t capacity = 1UL << ht_ptr->pfactor;
    size_t hop_range = ht_ptr->config.hop_range;
    HopscotchBucket *buckets = ht_ptr->buckets_ptr;

    /* Duplicate check (if enabled) */
    if (!ht_ptr->config.allow_duplicates) {
        if (sFindInNeighbourhood(ht_ptr, idx, key_ptr) != NULL) {
            return HOPSCOTCH_EDUPLICATE;
        }
    }

    /* Linear probe for an empty bucket */
    for (size_t i = idx; i < capacity; i++) {
        if (!buckets[i].data_ptr) {
            /* Displacement loop: while probe distance >= hop_range */
            size_t empty_idx = i;
            while (empty_idx - idx >= hop_range) {
                if (!sDisplaceForward(ht_ptr, idx, &empty_idx)) {
                    /* No displacement possible — resize and retry */
                    if (sResize(ht_ptr) != 0) {
                        return -1;  /* OOM during resize */
                    }
                    /* Retry insert after resize */
                    return HopscotchHashTableInsert(ht_ptr, data_ptr);
                }
            }

            /* Claim the empty slot */
            size_t off = empty_idx - idx;
            buckets[empty_idx].data_ptr = data_ptr;
            buckets[idx].hopinfo |= (1UL << off);
            ht_ptr->entry_count++;
            return 0;
        }
    }

    /* Table full — try resize, then retry */
    if (sResize(ht_ptr) == 0) {
        return HopscotchHashTableInsert(ht_ptr, data_ptr);
    }
    return -1;
}

PUBLIC_API void *
HopscotchHashTableLookup(HopscotchHashTable *ht_ptr, const uint8_t *key_ptr)
{
    if (!ht_ptr || !key_ptr) {
        return NULL;
    }

    size_t idx = sHashToIndex(ht_ptr, key_ptr);
    return sFindInNeighbourhood(ht_ptr, idx, key_ptr);
}

PUBLIC_API void *
HopscotchHashTableRemove(HopscotchHashTable *ht_ptr, const uint8_t *key_ptr)
{
    if (!ht_ptr || !key_ptr) {
        return NULL;
    }

    size_t idx = sHashToIndex(ht_ptr, key_ptr);
    size_t hop_range = ht_ptr->config.hop_range;
    HopscotchBucket *buckets = ht_ptr->buckets_ptr;

    if (!buckets[idx].hopinfo) {
        return NULL;
    }

    for (size_t i = 0; i < hop_range; i++) {
        if (buckets[idx].hopinfo & (1UL << i)) {
            void *candidate = buckets[idx + i].data_ptr;
            if (candidate) {
                const uint8_t *candidate_key = sExtractKey(ht_ptr, candidate);
                if (0 == sKeyCompare(ht_ptr, key_ptr, candidate_key)) {
                    void *data = candidate;
                    buckets[idx].hopinfo &= ~(1UL << i);
                    buckets[idx + i].data_ptr = NULL;
                    ht_ptr->entry_count--;
                    return data;
                }
            }
        }
    }
    return NULL;
}

/* ── Query ────────────────────────────────────────────────────────────── */

PUBLIC_API size_t
HopscotchHashTableSize(const HopscotchHashTable *ht_ptr)
{
    return ht_ptr ? ht_ptr->entry_count : 0;
}

PUBLIC_API size_t
HopscotchHashTableCapacity(const HopscotchHashTable *ht_ptr)
{
    return ht_ptr ? (1UL << ht_ptr->pfactor) : 0;
}

PUBLIC_API bool
HopscotchHashTableIsEmpty(const HopscotchHashTable *ht_ptr)
{
    return ht_ptr ? (ht_ptr->entry_count == 0) : true;
}

/* ── Iteration ────────────────────────────────────────────────────────── */

PUBLIC_API size_t
HopscotchHashTableForEach(HopscotchHashTable *ht_ptr,
                           HopscotchHashTableForEachCallback callback_ptr,
                           void *ctx_ptr)
{
    if (!ht_ptr || !callback_ptr) {
        return 0;
    }

    size_t visited = 0;
    size_t capacity = 1UL << ht_ptr->pfactor;
    HopscotchBucket *buckets = ht_ptr->buckets_ptr;

    for (size_t i = 0; i < capacity; i++) {
        if (buckets[i].data_ptr) {
            visited++;
            if (!callback_ptr(ctx_ptr, buckets[i].data_ptr)) {
                break;  /* Early termination */
            }
        }
    }

    return visited;
}
