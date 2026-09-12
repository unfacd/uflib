/**
 * Copyright (C) 2015-2026 unfacd works
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

// distinct_array_tests.cpp
// Hardened gtest suite for DistinctArray: put/lookup, duplicates, growth,
// iteration contents, ownership (slab copy), RemoveLast, error paths.

#include "gtest/gtest.h"
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <set>
#include <string>

extern "C" {
#include <uflib/adt/adt_distinct_array.h>
#include <uflib/adt/adt_hashtable.h>
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void
SetupDistinctArray(DistinctArray *da, size_t item_sz)
{
    memset(da, 0, sizeof(*da));
    da->distinct_array_descriptor.block_storage_unit_sz = item_sz;
    // offset past the historical size field; implementation treats the
    // allocation as a flat byte buffer of unit-sized slots
    da->distinct_array_descriptor.storage_slot_offset = sizeof(size_t);

    HashTableInstantiate(&da->hashTable,
                         HASHTABLE_ITEM_CONTAINER_OFFSET_ZERO, KEY_SIZE_ZERO,
                         HASH_ITEM_NOT_PTR_TYPE, "TestDistinctArray",
                         HASHTABLE_DEFAULT_EXTRACTOR);
}

// C++ nested-struct scoping requires the cast; ABI is identical to C.
static int
sPut(DistinctArray *da, uint8_t *item, size_t item_sz)
{
    return DistinctArrayPut(da,
                            (struct VariableBlockIndex **)&da->stored_value_idx,
                            (uint8_t **)&da->value_block_storage,
                            item, item_sz);
}

// =========================================================================
// 1 — Basic put + lookup round-trip
// =========================================================================

TEST(distinct_array, put_single_item_then_lookup)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *item = "hello";
    int rc = sPut(&da, (uint8_t *)item, strlen(item) + 1);
    EXPECT_EQ(rc, DistinctArrayResult_Ok);

    EXPECT_TRUE(IS_PRESENT(
        DistinctArrayLookupItem(&da, (uint8_t *)item)));

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, lookup_absent_item_returns_null)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *item = "present";
    sPut(&da, (uint8_t *)item, strlen(item) + 1);

    EXPECT_TRUE(IS_EMPTY(
        DistinctArrayLookupItem(&da, (uint8_t *)"absent")));

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 2 — Duplicate detection
// =========================================================================

TEST(distinct_array, duplicate_returns_duplicate_code)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *item = "unique";
    int rc1 = sPut(&da, (uint8_t *)item, strlen(item) + 1);
    EXPECT_EQ(rc1, DistinctArrayResult_Ok);

    int rc2 = sPut(&da, (uint8_t *)item, strlen(item) + 1);
    EXPECT_EQ(rc2, DistinctArrayResult_Duplicate);

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, duplicate_does_not_increase_array_size)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *item = "only_once";
    sPut(&da, (uint8_t *)item, strlen(item) + 1);
    EXPECT_EQ(da.stored_value_idx->size, 1U);

    sPut(&da, (uint8_t *)item, strlen(item) + 1);
    EXPECT_EQ(da.stored_value_idx->size, 1U);

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 3 — Multiple items
// =========================================================================

TEST(distinct_array, multiple_items_all_retrievable)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *items[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
    const int n = 5;

    for (int i = 0; i < n; i++) {
        int rc = sPut(&da, (uint8_t *)items[i], strlen(items[i]) + 1);
        EXPECT_EQ(rc, DistinctArrayResult_Ok) << "item: " << items[i];
    }

    EXPECT_EQ(da.stored_value_idx->size, (size_t)n);

    for (int i = 0; i < n; i++) {
        EXPECT_TRUE(IS_PRESENT(
            DistinctArrayLookupItem(&da, (uint8_t *)items[i])))
            << "missing: " << items[i];
    }

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 4 — IsItemStored convenience wrapper
// =========================================================================

TEST(distinct_array, is_item_stored_true_and_false)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    EXPECT_FALSE(DistinctArrayIsItemStored(&da, (uint8_t *)"nope"));

    sPut(&da, (uint8_t *)"yep", 4);

    EXPECT_TRUE(DistinctArrayIsItemStored(&da, (uint8_t *)"yep"));
    EXPECT_FALSE(DistinctArrayIsItemStored(&da, (uint8_t *)"nope"));

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 5 — Iteration (hardened: exact count + content verification)
// =========================================================================

TEST(distinct_array, iteration_visits_all_items)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *items[] = {"one", "two", "three"};
    const int n = 3;
    for (int i = 0; i < n; i++) {
        sPut(&da, (uint8_t *)items[i], strlen(items[i]) + 1);
    }

    __block int visited = 0;
    DistinctArrayIterate(&da, ^(uint8_t *item) {
        visited++;
    });

    EXPECT_EQ(visited, n);

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, iteration_yields_correct_contents)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *items[] = {"apple", "banana", "cherry"};
    const int n = 3;
    for (int i = 0; i < n; i++) {
        sPut(&da, (uint8_t *)items[i], strlen(items[i]) + 1);
    }

    __block std::set<std::string> seen;
    DistinctArrayIterate(&da, ^(uint8_t *item) {
        seen.insert(std::string((char *)item));
    });

    EXPECT_EQ(seen.size(), (size_t)n);
    EXPECT_TRUE(seen.count("apple"));
    EXPECT_TRUE(seen.count("banana"));
    EXPECT_TRUE(seen.count("cherry"));

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, iteration_on_empty_does_nothing)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    __block int visited = 0;
    DistinctArrayIterate(&da, ^(uint8_t *item) {
        visited++;
    });

    EXPECT_EQ(visited, 0);

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 6 — Slab growth (power-of-2 realloc)
// =========================================================================

TEST(distinct_array, slab_grows_beyond_initial_allocation)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    // power-of-2 triggers at 0→1, 1→2, 3→4, 7→8, 15→16, ...
    char buf[32];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "item-%02d", i);
        int rc = sPut(&da, (uint8_t *)buf, strlen(buf) + 1);
        EXPECT_EQ(rc, DistinctArrayResult_Ok) << "item " << i << ": " << buf;
    }

    EXPECT_EQ(da.stored_value_idx->size, 20U);

    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "item-%02d", i);
        EXPECT_TRUE(IS_PRESENT(
            DistinctArrayLookupItem(&da, (uint8_t *)buf)))
            << "missing: " << buf;
    }

    DistinctArrayDestruct(&da);
}

// FIX regression: realloc() may relocate the slab while it grows.  value_index[]
// and the hash table store raw pointers into the slab, so a move leaves them
// dangling (heap-use-after-free on the next lookup / iteration).  This test
// forces multiple growths and asserts each stored pointer lands in the exact
// slot of the *current* slab, and that every item stays retrievable.
TEST(distinct_array, slab_realloc_rebases_stored_pointers)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    // power-of-2 growth at 0→1, 1→2, 3→4, 7→8, 15→16 forces several reallocs.
    const int n = 20;
    char buf[32];
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "item-%02d", i);
        int rc = sPut(&da, (uint8_t *)buf, strlen(buf) + 1);
        EXPECT_EQ(rc, DistinctArrayResult_Ok) << "item " << i << ": " << buf;
    }

    // Hash table survived the re-bucketing — every item still found.
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "item-%02d", i);
        EXPECT_TRUE(IS_PRESENT(
            DistinctArrayLookupItem(&da, (uint8_t *)buf)))
            << "missing: " << buf;
    }

    // Each stored pointer is the exact i-th slot of the *current* slab.
    const uint8_t *block_base = (const uint8_t *)da.value_block_storage;
    size_t         offset     = da.distinct_array_descriptor.storage_slot_offset;
    size_t         unit       = da.distinct_array_descriptor.block_storage_unit_sz;
    for (int i = 0; i < n; i++) {
        EXPECT_EQ((const void *)da.stored_value_idx->value_index[i],
                  (const void *)(block_base + offset + unit * (size_t)i));
    }

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 7 — Empty array / edge cases
// =========================================================================

TEST(distinct_array, empty_array_lookup_returns_null)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    EXPECT_TRUE(IS_EMPTY(
        DistinctArrayLookupItem(&da, (uint8_t *)"nothing")));

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, empty_array_is_item_stored_returns_false)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    EXPECT_FALSE(DistinctArrayIsItemStored(&da, (uint8_t *)"nothing"));

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 8 — Destruct safety
// =========================================================================

TEST(distinct_array, destruct_on_empty_array)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, destruct_after_inserts)
{
    DistinctArray da;
    SetupDistinctArray(&da, 64);

    for (int i = 0; i < 5; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "key-%d", i);
        sPut(&da, (uint8_t *)buf, strlen(buf) + 1);
    }

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 9 — Result enum values
// =========================================================================

TEST(distinct_array, result_enum_values)
{
    EXPECT_EQ(DistinctArrayResult_Ok, 0);
    EXPECT_EQ(DistinctArrayResult_Duplicate, 1);
    EXPECT_EQ(DistinctArrayResult_HashError, 2);
}

// =========================================================================
// 10 — Oversized entry rejected (FIX coverage)
// =========================================================================

TEST(distinct_array, entry_larger_than_unit_returns_hash_error)
{
    DistinctArray da;
    SetupDistinctArray(&da, 8);   // tiny slots

    const char *big = "this_string_is_longer_than_eight_bytes";
    int rc = sPut(&da, (uint8_t *)big, strlen(big) + 1);
    EXPECT_EQ(rc, DistinctArrayResult_HashError);
    EXPECT_TRUE(da.stored_value_idx == NULL || da.stored_value_idx->size == 0U);

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 11 — Ownership / independent copy (FIX coverage)
// =========================================================================

TEST(distinct_array, put_copies_data_original_may_change)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    char buf[16] = "original";
    int rc = sPut(&da, (uint8_t *)buf, strlen(buf) + 1);
    EXPECT_EQ(rc, DistinctArrayResult_Ok);

    // mutate caller's buffer after Put — array must retain its copy
    buf[0] = 'X';

    EXPECT_TRUE(IS_PRESENT(
        DistinctArrayLookupItem(&da, (uint8_t *)"original")));
    EXPECT_FALSE(DistinctArrayIsItemStored(&da, (uint8_t *)"Xriginal"));

    void *removed = DistinctArrayRemoveLast(&da);
    EXPECT_NE(removed, nullptr);
    EXPECT_STREQ((char *)removed, "original");
    // do NOT free(removed) — it points into the slab

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 12 — Binary data
// =========================================================================

TEST(distinct_array, binary_data_as_items)
{
    DistinctArray da;
    SetupDistinctArray(&da, 16);

    uint64_t val1 = 42;
    uint64_t val2 = 99;
    uint64_t val3 = 42;

    int rc1 = sPut(&da, (uint8_t *)&val1, sizeof(val1));
    EXPECT_EQ(rc1, DistinctArrayResult_Ok);

    int rc2 = sPut(&da, (uint8_t *)&val2, sizeof(val2));
    EXPECT_EQ(rc2, DistinctArrayResult_Ok);

    int rc3 = sPut(&da, (uint8_t *)&val3, sizeof(val3));
    EXPECT_EQ(rc3, DistinctArrayResult_Duplicate);

    EXPECT_EQ(da.stored_value_idx->size, 2U);

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 13 — RemoveLast: basic behaviour
// =========================================================================

TEST(distinct_array, remove_last_from_single_item_returns_item_and_empties_array)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *item = "only";
    sPut(&da, (uint8_t *)item, strlen(item) + 1);
    EXPECT_EQ(da.stored_value_idx->size, 1U);

    void *removed = DistinctArrayRemoveLast(&da);
    EXPECT_NE(removed, nullptr);
    EXPECT_STREQ((char *)removed, "only");
    EXPECT_EQ(da.stored_value_idx->size, 0U);

    EXPECT_TRUE(IS_EMPTY(
        DistinctArrayLookupItem(&da, (uint8_t *)item)));

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, remove_last_from_multi_item_reduces_size)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    sPut(&da, (uint8_t *)"first", 6);
    sPut(&da, (uint8_t *)"second", 7);
    sPut(&da, (uint8_t *)"third", 6);
    EXPECT_EQ(da.stored_value_idx->size, 3U);

    void *removed = DistinctArrayRemoveLast(&da);
    EXPECT_NE(removed, nullptr);
    EXPECT_STREQ((char *)removed, "third");
    EXPECT_EQ(da.stored_value_idx->size, 2U);

    EXPECT_TRUE(IS_EMPTY(
        DistinctArrayLookupItem(&da, (uint8_t *)"third")));
    EXPECT_TRUE(IS_PRESENT(
        DistinctArrayLookupItem(&da, (uint8_t *)"first")));
    EXPECT_TRUE(IS_PRESENT(
        DistinctArrayLookupItem(&da, (uint8_t *)"second")));

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, remove_last_from_empty_returns_null)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    void *removed = DistinctArrayRemoveLast(&da);
    EXPECT_EQ(removed, nullptr);

    removed = DistinctArrayRemoveLast(&da);
    EXPECT_EQ(removed, nullptr);

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 14 — RemoveLast: complete emptying and re-fill
// =========================================================================

TEST(distinct_array, drain_all_items_then_refill)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *items[] = {"a", "b", "c", "d", "e"};
    const int n = 5;
    for (int i = 0; i < n; i++) {
        sPut(&da, (uint8_t *)items[i], strlen(items[i]) + 1);
    }
    EXPECT_EQ(da.stored_value_idx->size, 5U);

    int drained = 0;
    void *item;
    while ((item = DistinctArrayRemoveLast(&da))) {
        drained++;
        // do not free — slab-owned
    }
    EXPECT_EQ(drained, 5);
    EXPECT_EQ(da.stored_value_idx->size, 0U);

    for (int i = 0; i < n; i++) {
        EXPECT_TRUE(IS_EMPTY(
            DistinctArrayLookupItem(&da, (uint8_t *)items[i])));
    }

    const char *new_items[] = {"x", "y", "z"};
    for (int i = 0; i < 3; i++) {
        int rc = sPut(&da, (uint8_t *)new_items[i],
                       strlen(new_items[i]) + 1);
        EXPECT_EQ(rc, DistinctArrayResult_Ok);
    }
    EXPECT_EQ(da.stored_value_idx->size, 3U);

    for (int i = 0; i < 3; i++) {
        EXPECT_TRUE(IS_PRESENT(
            DistinctArrayLookupItem(&da, (uint8_t *)new_items[i])));
    }
    for (int i = 0; i < n; i++) {
        EXPECT_TRUE(IS_EMPTY(
            DistinctArrayLookupItem(&da, (uint8_t *)items[i])));
    }

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, drain_and_refill_multiple_cycles)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    const char *items[] = {"c0-a", "c0-b", "c0-c", "c0-d",
                           "c1-a", "c1-b", "c1-c", "c1-d",
                           "c2-a", "c2-b", "c2-c", "c2-d"};

    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < 4; i++) {
            const char *val = items[cycle * 4 + i];
            sPut(&da, (uint8_t *)val, strlen(val) + 1);
        }
        EXPECT_EQ(da.stored_value_idx->size, 4U);

        int drained = 0;
        while (DistinctArrayRemoveLast(&da)) drained++;
        EXPECT_EQ(drained, 4);
        EXPECT_EQ(da.stored_value_idx->size, 0U);
    }

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, remove_last_preserves_allocation_no_shrink)
{
    DistinctArray da;
    SetupDistinctArray(&da, 16);

    char keys[30][16];
    for (int i = 0; i < 30; i++) {
        snprintf(keys[i], sizeof(keys[i]), "k%d", i);
        sPut(&da, (uint8_t *)keys[i], strlen(keys[i]) + 1);
    }
    size_t block_sz_before = da.distinct_array_descriptor.block_allocated_sz;
    size_t coll_sz_before  = da.distinct_array_descriptor.collection_allocated_sz;

    while (DistinctArrayRemoveLast(&da)) {}

    EXPECT_EQ(da.stored_value_idx->size, 0U);
    EXPECT_EQ(da.distinct_array_descriptor.block_allocated_sz, block_sz_before);
    EXPECT_EQ(da.distinct_array_descriptor.collection_allocated_sz, coll_sz_before);

    const char *refill[] = {"r0", "r1", "r2", "r3", "r4",
                            "r5", "r6", "r7", "r8", "r9"};
    for (int i = 0; i < 10; i++) {
        int rc = sPut(&da, (uint8_t *)refill[i], strlen(refill[i]) + 1);
        EXPECT_EQ(rc, DistinctArrayResult_Ok);
    }
    EXPECT_EQ(da.stored_value_idx->size, 10U);

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 15 — RemoveLast: hash-table sync
// =========================================================================

TEST(distinct_array, removed_item_cannot_be_looked_up)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    sPut(&da, (uint8_t *)"alpha", 6);
    sPut(&da, (uint8_t *)"beta", 5);

    DistinctArrayRemoveLast(&da);  // removes "beta"

    EXPECT_TRUE(IS_EMPTY(
        DistinctArrayLookupItem(&da, (uint8_t *)"beta")));
    EXPECT_FALSE(
        DistinctArrayIsItemStored(&da, (uint8_t *)"beta"));

    EXPECT_TRUE(IS_PRESENT(
        DistinctArrayLookupItem(&da, (uint8_t *)"alpha")));

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, removed_item_can_be_reinserted)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    int rc = sPut(&da, (uint8_t *)"item", 5);
    EXPECT_EQ(rc, DistinctArrayResult_Ok);

    DistinctArrayRemoveLast(&da);

    rc = sPut(&da, (uint8_t *)"item", 5);
    EXPECT_EQ(rc, DistinctArrayResult_Ok);

    EXPECT_TRUE(IS_PRESENT(
        DistinctArrayLookupItem(&da, (uint8_t *)"item")));

    DistinctArrayDestruct(&da);
}

TEST(distinct_array, remove_last_leaves_no_hash_residue_for_iteration)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    sPut(&da, (uint8_t *)"keep", 5);
    sPut(&da, (uint8_t *)"discard", 8);
    sPut(&da, (uint8_t *)"keep2", 6);

    DistinctArrayRemoveLast(&da);  // removes "keep2"
    DistinctArrayRemoveLast(&da);  // removes "discard"

    __block int visited = 0;
    DistinctArrayIterate(&da, ^(uint8_t *item) {
        visited++;
        EXPECT_STREQ((char *)item, "keep");
    });
    EXPECT_EQ(visited, 1);

    EXPECT_TRUE(IS_PRESENT(
        DistinctArrayLookupItem(&da, (uint8_t *)"keep")));

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 16 — Interleaved remove and put
// =========================================================================

TEST(distinct_array, interleaved_remove_and_put)
{
    DistinctArray da;
    SetupDistinctArray(&da, 32);

    sPut(&da, (uint8_t *)"a", 2);
    sPut(&da, (uint8_t *)"b", 2);
    sPut(&da, (uint8_t *)"c", 2);
    EXPECT_EQ(da.stored_value_idx->size, 3U);

    DistinctArrayRemoveLast(&da);  // remove "c"
    EXPECT_EQ(da.stored_value_idx->size, 2U);

    sPut(&da, (uint8_t *)"d", 2);
    sPut(&da, (uint8_t *)"e", 2);
    EXPECT_EQ(da.stored_value_idx->size, 4U);

    DistinctArrayRemoveLast(&da);  // remove "e"
    EXPECT_EQ(da.stored_value_idx->size, 3U);

    EXPECT_TRUE(IS_PRESENT(DistinctArrayLookupItem(&da, (uint8_t *)"a")));
    EXPECT_TRUE(IS_PRESENT(DistinctArrayLookupItem(&da, (uint8_t *)"b")));
    EXPECT_TRUE(IS_PRESENT(DistinctArrayLookupItem(&da, (uint8_t *)"d")));
    EXPECT_TRUE(IS_EMPTY(DistinctArrayLookupItem(&da, (uint8_t *)"c")));
    EXPECT_TRUE(IS_EMPTY(DistinctArrayLookupItem(&da, (uint8_t *)"e")));

    DistinctArrayDestruct(&da);
}

// =========================================================================
// 17 — Varying item sizes (within unit)
// =========================================================================

TEST(distinct_array, varying_item_sizes)
{
    DistinctArray da;
    SetupDistinctArray(&da, 256);

    const char *short_str = "a";
    const char *long_str  = "this_is_a_much_longer_string_for_testing_purposes";

    int rc1 = sPut(&da, (uint8_t *)short_str, strlen(short_str) + 1);
    EXPECT_EQ(rc1, DistinctArrayResult_Ok);

    int rc2 = sPut(&da, (uint8_t *)long_str, strlen(long_str) + 1);
    EXPECT_EQ(rc2, DistinctArrayResult_Ok);

    EXPECT_TRUE(IS_PRESENT(
        DistinctArrayLookupItem(&da, (uint8_t *)short_str)));
    EXPECT_TRUE(IS_PRESENT(
        DistinctArrayLookupItem(&da, (uint8_t *)long_str)));

    DistinctArrayDestruct(&da);
}
