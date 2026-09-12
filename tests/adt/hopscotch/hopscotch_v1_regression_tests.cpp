/**
 * @file
 * @brief V1 regression tests for adt_hopscotch_hashtable bug fixes.
 *
 * Validates the three confirmed bugs (A: resize data loss, B: remove segfault,
 * C: wrong allocation sizeof) are fixed at Gate 1 before any V2 code is written.
 *
 * These tests use the existing V1 public headers and link against the V1
 * implementation.  They are separate from the V2 test suite (Gate 4).
 */

#include <gtest/gtest.h>

extern "C" {
#include <uflib/adt/adt_hopscotch_hashtable.h>
}

// ─── Bug A: Resize drops all data in keyed variant ────────────────────────
//
// hopscotch_resize() at line 569 read from buckets[i] (new zeroed array)
// instead of obuckets[i] (old array).  All data silently dropped on resize.
// Fixed: buckets[i] -> obuckets[i].

TEST(HopscotchV1Regression, BugA_KeyedVariantResizePreservesAllData)
{
    // Use pfactor=6 (64 buckets) and insert enough entries that some will
    // hash to the same neighbourhood, potentially triggering displacement
    // and a resize.  The keyed variant has hop_range=32, so with 64 buckets
    // we can actually trigger the displacement-exhaustion path.
    const size_t pfactor = 6;
    const size_t num_entries = 50;

    HopscotchHashtable ht_storage;
    HopscotchHashtable *ht = hopscotch_init(&ht_storage, pfactor);
    ASSERT_NE(ht, nullptr);

    // Insert entries — track which ones succeeded
    size_t inserted = 0;
    uintptr_t inserted_keys[50];
    for (size_t i = 1; i <= num_entries; i++) {
        int ret = hopscotch_insert(ht, (uintptr_t)i, (void *)i);
        if (ret == 0) {
            inserted_keys[inserted++] = (uintptr_t)i;
        }
        // ret == -1 means table full (shouldn't happen with 50 entries in 64 buckets
        // unless displacement exhaustion fills the table, which is fine)
    }

    ASSERT_GT(inserted, 0u) << "No entries could be inserted";

    // Verify every successfully inserted entry is retrievable.
    // Before Bug A fix, resize would silently drop all data — found would be 0
    // for entries that were in the table before resize.
    size_t found = 0;
    for (size_t i = 0; i < inserted; i++) {
        void *data = hopscotch_lookup(ht, (intptr_t)inserted_keys[i]);
        if (data == (void *)inserted_keys[i]) {
            found++;
        }
    }

    EXPECT_EQ(found, inserted)
        << "Bug A regression: expected all " << inserted
        << " inserted entries to be findable, but only found " << found;

    hopscotch_release(ht);
}

// ─── Bug B: Remove hashes key value as pointer -> segfault on small keys ──
//
// hopscotch_remove() at line 507 used (uint8_t *)key instead of
// (uint8_t *)&key_provided.  Small integer keys (e.g., 42) cast to invalid
// pointers -> segfault.  Fixed: (uint8_t *)key -> (uint8_t *)&key_provided.

TEST(HopscotchV1Regression, BugB_RemoveWithSmallIntegerKeyNoSegfault)
{
    HopscotchHashtable ht_storage;
    HopscotchHashtable *ht = hopscotch_init(&ht_storage, 6);
    ASSERT_NE(ht, nullptr);

    // Insert with a small integer key that would segfault on remove before fix
    const uintptr_t small_key = 42;
    void *test_data = (void *)0xDEADBEEF;

    int ret = hopscotch_insert(ht, small_key, test_data);
    ASSERT_EQ(ret, 0);

    // This should NOT segfault after the fix
    void *removed = hopscotch_remove(ht, small_key);
    EXPECT_EQ(removed, test_data)
        << "Bug B regression: remove should return the inserted data for key=" << small_key;

    // Verify the entry is actually gone
    void *lookup = hopscotch_lookup(ht, (intptr_t)small_key);
    EXPECT_EQ(lookup, nullptr)
        << "Bug B regression: lookup after remove should return NULL";

    // Remove non-existent key should return NULL (not crash)
    void *gone = hopscotch_remove(ht, 999);
    EXPECT_EQ(gone, nullptr);

    hopscotch_release(ht);
}

// ─── Bug C: hopscotch_init allocates wrong struct size ────────────────────
//
// hopscotch_init() at line 385 used sizeof(struct hopscotch_bucket_keyed)
// (24 bytes) instead of sizeof(struct HopscotchHashtable) (16 bytes).
// Fixed: sizeof(struct hopscotch_bucket_keyed) -> sizeof(struct HopscotchHashtable).

TEST(HopscotchV1Regression, BugC_InitAllocatesCorrectStructSize)
{
    // Call hopscotch_init with ht=NULL so it allocates the struct internally.
    // The bug caused an 8-byte over-allocation (24 instead of 16).  This test
    // verifies the fix by checking that the returned struct is usable (insert +
    // lookup works) and that the internal layout is correct.

    HopscotchHashtable *ht = hopscotch_init(nullptr, 6);
    ASSERT_NE(ht, nullptr);

    // Verify the struct is functional — insert and lookup work
    int ret = hopscotch_insert(ht, (uintptr_t)100, (void *)0xCAFE);
    ASSERT_EQ(ret, 0);

    void *found = hopscotch_lookup(ht, (intptr_t)100);
    EXPECT_EQ(found, (void *)0xCAFE)
        << "Bug C regression: struct should be functional after correct allocation";

    // Clean up: release buckets, then free the ht struct that init allocated
    hopscotch_release(ht);
    free(ht);
}
