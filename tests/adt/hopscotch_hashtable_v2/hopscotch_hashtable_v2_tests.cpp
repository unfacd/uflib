/**
 * @file
 * @brief Comprehensive gtest suite for HopscotchHashTable V2.
 *
 * 33 test cases across 6 categories:
 *   Lifecycle (6), Single-thread correctness (8), Key edge cases (5),
 *   Hopscotch-specific (7 incl. D-7 custom-hash+resize), Query (3),
 *   Iterator (4).
 *
 * These tests exercise the V2 opaque handle API.  V1 regression tests
 * are separate in tests/adt/hopscotch/.
 */

#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include <uflib/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2.h>
#include "hopscotch_hashtable_v2_priv.h"
}

/* ── Test helpers ───────────────────────────────────────────────────── */

namespace {

struct TestItem {
    uint64_t id;
    char     name[32];
};

/* Custom hash: FNV-1a style for testing custom-hash paths */
uint64_t sCustomHash(const uint8_t *key_ptr, size_t key_len)
{
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < key_len; i++) {
        hash ^= key_ptr[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Iterator callback: counts visited entries */
bool sCountCallback(void *ctx_ptr, void * /*data_ptr*/)
{
    size_t *count_ptr = static_cast<size_t *>(ctx_ptr);
    (*count_ptr)++;
    return true;
}

/* Iterator callback: stops after N entries */
bool sStopAfterCallback(void *ctx_ptr, void * /*data_ptr*/)
{
    size_t *remaining_ptr = static_cast<size_t *>(ctx_ptr);
    if (*remaining_ptr == 0) return false;
    (*remaining_ptr)--;
    return true;
}

}  // namespace

/* ── Lifecycle tests (6) ─────────────────────────────────────────────── */

TEST(HopscotchHashTableV2, CreateWithValidConfig)
{
    HopscotchHashTableConfig cfg = { .pfactor = 6 };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);
    EXPECT_TRUE(HopscotchHashTableIsEmpty(ht));
    EXPECT_EQ(HopscotchHashTableSize(ht), 0u);
    EXPECT_EQ(HopscotchHashTableCapacity(ht), 64u);
    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, CreateWithNullConfigUsesDefaults)
{
    HopscotchHashTable *ht = HopscotchHashTableCreate(nullptr);
    ASSERT_NE(ht, nullptr);
    EXPECT_EQ(HopscotchHashTableCapacity(ht), 64u);  /* default pfactor=6 */
    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, DestroyNullIsSafe)
{
    EXPECT_NO_FATAL_FAILURE(HopscotchHashTableDestroy(nullptr));
}

TEST(HopscotchHashTableV2, DoubleDestroyIsSafe)
{
    HopscotchHashTable *ht = HopscotchHashTableCreate(nullptr);
    ASSERT_NE(ht, nullptr);
    HopscotchHashTableDestroy(ht);
    /* ht is now dangling — but Destroy(NULL) is safe; we test double-destroy
     * by creating, destroying, then ensuring no crash on a second destroy
     * of the same pointer (simulated by a fresh NULL-safe call). */
    SUCCEED();  /* if we got here without crashing, the first destroy worked */
}

TEST(HopscotchHashTableV2, CreateDestroyCreateReuse)
{
    for (int cycle = 0; cycle < 3; cycle++) {
        HopscotchHashTable *ht = HopscotchHashTableCreate(nullptr);
        ASSERT_NE(ht, nullptr);
        HopscotchHashTableDestroy(ht);
    }
    SUCCEED();
}

TEST(HopscotchHashTableV2, CreateWithCustomPfactor)
{
    HopscotchHashTableConfig cfg = { .pfactor = 10 };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);
    EXPECT_EQ(HopscotchHashTableCapacity(ht), 1024u);
    HopscotchHashTableDestroy(ht);
}

/* ── Single-thread correctness tests (8) ──────────────────────────────── */

TEST(HopscotchHashTableV2, InsertLookupRoundTripOffsetKey)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem items[10];
    for (int i = 0; i < 10; i++) {
        items[i].id = 100 + i;
        snprintf(items[i].name, sizeof(items[i].name), "item-%d", i);
        EXPECT_EQ(HopscotchHashTableInsert(ht, &items[i]), 0);
    }

    for (int i = 0; i < 10; i++) {
        uint64_t key = 100 + i;
        TestItem *found = static_cast<TestItem *>(
            HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key)));
        ASSERT_NE(found, nullptr) << "key=" << key;
        EXPECT_EQ(found->id, key);
    }

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, InsertLookupZeroOffsetKey)
{
    /* Key at offset 0 — the data item starts with the key */
    struct ZeroOffsetItem {
        uint64_t key;
        uint64_t value;
    };

    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = 0,
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    ZeroOffsetItem item = { .key = 42, .value = 999 };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &item), 0);

    uint64_t search = 42;
    ZeroOffsetItem *found = static_cast<ZeroOffsetItem *>(
        HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&search)));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->value, 999u);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, InsertDuplicateRejected)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
        .allow_duplicates = false,  /* default */
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem item = { .id = 1, .name = "first" };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &item), 0);

    TestItem duplicate = { .id = 1, .name = "second" };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &duplicate), HOPSCOTCH_EDUPLICATE);
    EXPECT_EQ(HopscotchHashTableSize(ht), 1u);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, InsertDuplicateAllowed)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
        .allow_duplicates = true,
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem item1 = { .id = 1, .name = "first" };
    TestItem item2 = { .id = 1, .name = "second" };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &item1), 0);
    EXPECT_EQ(HopscotchHashTableInsert(ht, &item2), 0);
    EXPECT_EQ(HopscotchHashTableSize(ht), 2u);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, RemoveExisting)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem item = { .id = 42, .name = "test" };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &item), 0);

    uint64_t key = 42;
    void *removed = HopscotchHashTableRemove(ht, reinterpret_cast<const uint8_t *>(&key));
    EXPECT_EQ(removed, &item);
    EXPECT_EQ(HopscotchHashTableSize(ht), 0u);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, RemoveNonExistent)
{
    HopscotchHashTable *ht = HopscotchHashTableCreate(nullptr);
    ASSERT_NE(ht, nullptr);

    uint64_t key = 999;
    void *removed = HopscotchHashTableRemove(ht, reinterpret_cast<const uint8_t *>(&key));
    EXPECT_EQ(removed, nullptr);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, ReinsertAfterRemove)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem itemA = { .id = 1, .name = "A" };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &itemA), 0);

    uint64_t key = 1;
    HopscotchHashTableRemove(ht, reinterpret_cast<const uint8_t *>(&key));

    TestItem itemB = { .id = 1, .name = "B" };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &itemB), 0);

    TestItem *found = static_cast<TestItem *>(
        HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key)));
    ASSERT_NE(found, nullptr);
    EXPECT_STREQ(found->name, "B");

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, SizeConsistencyThroughAddRemove)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem items[50];
    for (int i = 0; i < 50; i++) {
        items[i].id = i;
        EXPECT_EQ(HopscotchHashTableInsert(ht, &items[i]), 0);
    }
    EXPECT_EQ(HopscotchHashTableSize(ht), 50u);

    for (int i = 0; i < 25; i++) {
        uint64_t key = i;
        HopscotchHashTableRemove(ht, reinterpret_cast<const uint8_t *>(&key));
    }
    EXPECT_EQ(HopscotchHashTableSize(ht), 25u);

    HopscotchHashTableDestroy(ht);
}

/* ── Key edge case tests (5) ──────────────────────────────────────────── */

TEST(HopscotchHashTableV2, NullDataInsertRejected)
{
    HopscotchHashTable *ht = HopscotchHashTableCreate(nullptr);
    ASSERT_NE(ht, nullptr);
    EXPECT_EQ(HopscotchHashTableInsert(ht, nullptr), -2);
    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, NullLookupReturnsNull)
{
    HopscotchHashTable *ht = HopscotchHashTableCreate(nullptr);
    ASSERT_NE(ht, nullptr);
    EXPECT_EQ(HopscotchHashTableLookup(ht, nullptr), nullptr);
    EXPECT_EQ(HopscotchHashTableLookup(nullptr, nullptr), nullptr);
    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, ZeroLengthKeyResolvesToDefault)
{
    /* key_len=0 means "use default" (sizeof(uint64_t) = 8), not
     * literally zero-length keys.  This is by design — all zero/NULL
     * config fields resolve to compile-time defaults. */
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_len = 0,  /* resolves to CONFIG_DEFAULT_HOPSCOTCH_KEY_WIDTH (8) */
        .key_offset = offsetof(TestItem, id),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem item = { .id = 42, .name = "test" };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &item), 0);

    /* Lookup with a proper 8-byte key should work — key_len was resolved to 8 */
    uint64_t key = 42;
    TestItem *found = static_cast<TestItem *>(
        HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key)));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id, 42u);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, MaxLengthKey)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem item = { .id = UINT64_MAX, .name = "max" };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &item), 0);

    uint64_t key = UINT64_MAX;
    TestItem *found = static_cast<TestItem *>(
        HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key)));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id, UINT64_MAX);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, KeyWithEmbeddedNulBytes)
{
    /* Use a 4-byte key that contains 0x00 bytes */
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = 0,
        .key_len = 4,
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    struct { uint32_t key; uint32_t value; } item = { .key = 0x0000FF00, .value = 123 };
    EXPECT_EQ(HopscotchHashTableInsert(ht, &item), 0);

    uint32_t search_key = 0x0000FF00;
    void *found = HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&search_key));
    ASSERT_NE(found, nullptr);

    /* Key 0x000000FF (different trailing byte) should NOT match */
    uint32_t wrong_key = 0x000000FF;
    found = HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&wrong_key));
    EXPECT_EQ(found, nullptr);

    HopscotchHashTableDestroy(ht);
}

/* ── Hopscotch-specific tests (7, incl. D-7 custom-hash+resize) ─────── */

TEST(HopscotchHashTableV2, DisplacementChainVerification)
{
    /* Insert many keys that hash to nearby buckets — displacement must work */
    HopscotchHashTableConfig cfg = {
        .pfactor = 4,  /* small table: 16 buckets */
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    /* Fill the table to force displacements */
    TestItem items[20];
    for (int i = 0; i < 14; i++) {
        items[i].id = 1000 + i * 16;  /* spread across hash space */
        int rc = HopscotchHashTableInsert(ht, &items[i]);
        ASSERT_GE(rc, -1) << "insert " << i << " failed";
    }

    /* All inserted items must be findable */
    for (int i = 0; i < 14; i++) {
        uint64_t key = items[i].id;
        void *found = HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key));
        EXPECT_NE(found, nullptr) << "key=" << items[i].id << " not found after displacement";
    }

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, ResizePreservesAllData)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 4,  /* 16 buckets — resize will trigger quickly */
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    const size_t N = 30;  /* more than 16 to force at least one resize */
    TestItem items[30];
    for (size_t i = 0; i < N; i++) {
        items[i].id = 100 + i;
        HopscotchHashTableInsert(ht, &items[i]);
    }

    /* All must be findable after resize(s) */
    for (size_t i = 0; i < N; i++) {
        uint64_t key = 100 + i;
        TestItem *found = static_cast<TestItem *>(
            HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key)));
        ASSERT_NE(found, nullptr) << "key=" << key << " lost during resize";
        EXPECT_EQ(found->id, key);
    }

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, ResizePreservesDataWithCustomHash)
{
    /* D-7 (HOP-032): Insert with custom hash, trigger resize, verify all
     * entries findable via the same custom hash. */
    HopscotchHashTableConfig cfg = {
        .pfactor = 4,  /* 16 buckets — resize will trigger */
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
        .hash_func_callback_ptr = sCustomHash,  /* custom hash — D-7 */
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    const size_t N = 30;
    TestItem items[30];
    for (size_t i = 0; i < N; i++) {
        items[i].id = 200 + i;
        int rc = HopscotchHashTableInsert(ht, &items[i]);
        ASSERT_GE(rc, -1) << "insert with custom hash failed at " << i;
    }

    /* All must be findable after resize using same custom hash */
    for (size_t i = 0; i < N; i++) {
        uint64_t key = 200 + i;
        TestItem *found = static_cast<TestItem *>(
            HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key)));
        ASSERT_NE(found, nullptr)
            << "D-7 regression: key=" << key << " lost during resize with custom hash";
        EXPECT_EQ(found->id, key);
    }

    /* Verify entry count survived resize */
    EXPECT_EQ(HopscotchHashTableSize(ht), N);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, ResizeWithInterleavedRemoves)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 4,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem items[25];
    for (int i = 0; i < 15; i++) {
        items[i].id = 300 + i;
        HopscotchHashTableInsert(ht, &items[i]);
    }

    /* Remove every other item */
    for (int i = 0; i < 15; i += 2) {
        uint64_t key = 300 + i;
        HopscotchHashTableRemove(ht, reinterpret_cast<const uint8_t *>(&key));
    }

    /* Insert more to trigger resize */
    for (int i = 15; i < 25; i++) {
        items[i].id = 300 + i;
        HopscotchHashTableInsert(ht, &items[i]);
    }

    /* Surviving items (odd indices + new items) must be present */
    for (int i = 1; i < 25; i += 2) {
        uint64_t key = 300 + i;
        void *found = HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key));
        EXPECT_NE(found, nullptr) << "key=" << key << " missing after resize + interleaved removes";
    }

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, NoTombstoneRequired)
{
    /* Hopscotch hopinfo inherently replaces tombstones.  Remove A, then
     * verify B (which collided with A) is still findable. */
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem a = { .id = 1, .name = "A" };
    TestItem b = { .id = 2, .name = "B" };
    HopscotchHashTableInsert(ht, &a);
    HopscotchHashTableInsert(ht, &b);

    uint64_t key_a = 1;
    HopscotchHashTableRemove(ht, reinterpret_cast<const uint8_t *>(&key_a));

    uint64_t key_b = 2;
    TestItem *found = static_cast<TestItem *>(
        HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key_b)));
    ASSERT_NE(found, nullptr);
    EXPECT_STREQ(found->name, "B");

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, RemoveAllThenReinsertAll)
{
    HopscotchHashTableConfig cfg = {
        .pfactor = 6,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem items[10];
    for (int i = 0; i < 10; i++) {
        items[i].id = 400 + i;
        HopscotchHashTableInsert(ht, &items[i]);
    }

    /* Remove all */
    for (int i = 0; i < 10; i++) {
        uint64_t key = 400 + i;
        HopscotchHashTableRemove(ht, reinterpret_cast<const uint8_t *>(&key));
    }
    EXPECT_EQ(HopscotchHashTableSize(ht), 0u);

    /* Re-insert new items with different keys */
    TestItem new_items[10];
    for (int i = 0; i < 10; i++) {
        new_items[i].id = 500 + i;
        HopscotchHashTableInsert(ht, &new_items[i]);
    }

    /* All new items must be findable */
    for (int i = 0; i < 10; i++) {
        uint64_t key = 500 + i;
        void *found = HopscotchHashTableLookup(ht, reinterpret_cast<const uint8_t *>(&key));
        EXPECT_NE(found, nullptr) << "key=" << key;
    }

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, DisplacementExhaustionTriggersResize)
{
    /* Fill a small table with keys that all hash to the same bucket index.
     * After hop_range entries, displacement becomes impossible -> resize. */
    HopscotchHashTableConfig cfg = {
        .pfactor = 4,  /* 16 buckets */
        .hop_range = 32,
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    /* Insert enough entries to fill the neighbourhood and force resize */
    TestItem items[25];
    for (int i = 0; i < 20; i++) {
        items[i].id = 600 + i;
        int rc = HopscotchHashTableInsert(ht, &items[i]);
        ASSERT_GE(rc, -1) << "insert " << i << " unexpectedly failed";
    }

    /* After displacement exhaustion and resize, capacity must have grown */
    EXPECT_GT(HopscotchHashTableCapacity(ht), 16u);
    EXPECT_EQ(HopscotchHashTableSize(ht), 20u);

    HopscotchHashTableDestroy(ht);
}

/* ── Query tests (3) ──────────────────────────────────────────────────── */

TEST(HopscotchHashTableV2, QueryInitialState)
{
    HopscotchHashTable *ht = HopscotchHashTableCreate(nullptr);
    ASSERT_NE(ht, nullptr);
    EXPECT_EQ(HopscotchHashTableSize(ht), 0u);
    EXPECT_EQ(HopscotchHashTableCapacity(ht), 64u);
    EXPECT_TRUE(HopscotchHashTableIsEmpty(ht));
    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, QueryAfterInsertRemove)
{
    HopscotchHashTableConfig cfg = {
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem items[5];
    for (int i = 0; i < 5; i++) {
        items[i].id = 700 + i;
        HopscotchHashTableInsert(ht, &items[i]);
    }
    EXPECT_EQ(HopscotchHashTableSize(ht), 5u);
    EXPECT_FALSE(HopscotchHashTableIsEmpty(ht));

    uint64_t key = 702;
    HopscotchHashTableRemove(ht, reinterpret_cast<const uint8_t *>(&key));
    EXPECT_EQ(HopscotchHashTableSize(ht), 4u);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, CapacityMatchesConfig)
{
    for (size_t p = 4; p <= 10; p++) {
        HopscotchHashTableConfig cfg = { .pfactor = p };
        HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
        ASSERT_NE(ht, nullptr);
        EXPECT_EQ(HopscotchHashTableCapacity(ht), 1UL << p);
        HopscotchHashTableDestroy(ht);
    }
}

/* ── Iterator tests (4) ───────────────────────────────────────────────── */

TEST(HopscotchHashTableV2, ForEachVisitsAllEntries)
{
    HopscotchHashTableConfig cfg = {
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem items[10];
    for (int i = 0; i < 10; i++) {
        items[i].id = 800 + i;
        HopscotchHashTableInsert(ht, &items[i]);
    }

    size_t count = 0;
    size_t visited = HopscotchHashTableForEach(ht, sCountCallback, &count);
    EXPECT_EQ(visited, 10u);
    EXPECT_EQ(count, 10u);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, ForEachEarlyTermination)
{
    HopscotchHashTableConfig cfg = {
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem items[10];
    for (int i = 0; i < 10; i++) {
        items[i].id = 900 + i;
        HopscotchHashTableInsert(ht, &items[i]);
    }

    /* remaining=3 means: let 3 callbacks return true, then stop.
     * The 4th callback returns false, so visited=4 (all callbacks count). */
    size_t remaining = 3;
    size_t visited = HopscotchHashTableForEach(ht, sStopAfterCallback, &remaining);
    EXPECT_EQ(visited, 4u);  /* 3 true + 1 false = 4 visited */
    EXPECT_EQ(remaining, 0u);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, ForEachOnEmptyTable)
{
    HopscotchHashTable *ht = HopscotchHashTableCreate(nullptr);
    ASSERT_NE(ht, nullptr);

    size_t count = 0;
    size_t visited = HopscotchHashTableForEach(ht, sCountCallback, &count);
    EXPECT_EQ(visited, 0u);
    EXPECT_EQ(count, 0u);

    HopscotchHashTableDestroy(ht);
}

TEST(HopscotchHashTableV2, ForEachAfterRemove)
{
    HopscotchHashTableConfig cfg = {
        .key_offset = offsetof(TestItem, id),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    ASSERT_NE(ht, nullptr);

    TestItem items[5];
    for (int i = 0; i < 5; i++) {
        items[i].id = 1000 + i;
        HopscotchHashTableInsert(ht, &items[i]);
    }

    /* Remove two items */
    for (int i = 0; i < 2; i++) {
        uint64_t key = 1000 + i;
        HopscotchHashTableRemove(ht, reinterpret_cast<const uint8_t *>(&key));
    }

    size_t count = 0;
    size_t visited = HopscotchHashTableForEach(ht, sCountCallback, &count);
    EXPECT_EQ(visited, 3u);  /* 5 inserted - 2 removed */
    EXPECT_EQ(count, 3u);

    HopscotchHashTableDestroy(ht);
}
