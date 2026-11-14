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

/*
 * V1 configurable hopscotch store (adt_hopscotch_hashtable.h).
 *
 * The store is an identity set: the caller supplies hash_func, the table hashes
 * and compares the item pointer, and it grows by rehashing with that same
 * function.  These tests pin that contract, including the growth path, which is
 * the only place the table allocates after initialisation.
 */

#include <gtest/gtest.h>

extern "C" {
#include <uflib/adt/adt_hopscotch_hashtable.h>
}

namespace {

uint64_t spread_hash(const uint8_t *key, size_t key_len)
{
    uint64_t hash = 0xcbf29ce484222325ULL;

    for (size_t i = 0; i < key_len; i++) {
        hash ^= key[i];
        hash *= 0x100000001b3ULL;
    }

    return hash;
}

uint64_t constant_hash(const uint8_t *, size_t)
{
    return 0;
}

/* Deterministic PRNG so a failure is reproducible from the seed alone. */
struct Rng {
    uint64_t state;

    explicit Rng(uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ULL) {}

    uint64_t next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;

        return state;
    }

    size_t below(size_t limit) { return (size_t)(next() % limit); }
};

void *item(size_t idx)
{
    return (void *)(uintptr_t)(0x100000 + idx * 64);
}

class ConfigurableStoreTest : public ::testing::Test {
protected:
    HopscotchHashtableConfigurable store;

    void SetUp() override
    {
        memset(&store, 0, sizeof(store));
        ASSERT_NE(hopscotch_init_with_offset(&store.hashtable, 2), nullptr);
        store.hash_func = spread_hash;
    }

    void TearDown() override
    {
        hopscotch_release(&store.hashtable);
    }

    size_t buckets() const { return (size_t)1 << store.hashtable.pfactor; }

    int insert(void *ptr) { return hopscotch_insert_configurable(&store, (uint8_t *)ptr); }
    void *lookup(void *ptr) { return hopscotch_lookup_configurable(&store, (uint8_t *)ptr); }
    void *remove(void *ptr) { return hopscotch_remove_configurable(&store, (uint8_t *)ptr); }
};

TEST_F(ConfigurableStoreTest, InsertedItemIsFound)
{
    ASSERT_EQ(insert(item(1)), 0);
    EXPECT_EQ(lookup(item(1)), item(1));
}

TEST_F(ConfigurableStoreTest, AbsentItemIsNotFound)
{
    ASSERT_EQ(insert(item(1)), 0);
    EXPECT_EQ(lookup(item(2)), nullptr);
}

TEST_F(ConfigurableStoreTest, EmptyStoreFindsNothing)
{
    EXPECT_FALSE(store.hashtable.buckets == nullptr);
    EXPECT_EQ(lookup(item(1)), nullptr);
    EXPECT_EQ(remove(item(1)), nullptr);
}

TEST_F(ConfigurableStoreTest, NullItemIsRejected)
{
    EXPECT_EQ(insert(nullptr), -2);
}

TEST_F(ConfigurableStoreTest, RemoveReturnsItemAndErasesIt)
{
    ASSERT_EQ(insert(item(1)), 0);
    ASSERT_EQ(insert(item(2)), 0);

    EXPECT_EQ(remove(item(1)), item(1));
    EXPECT_EQ(lookup(item(1)), nullptr);
    EXPECT_EQ(lookup(item(2)), item(2));

    EXPECT_EQ(remove(item(1)), nullptr);
}

TEST_F(ConfigurableStoreTest, ReinsertAfterRemoveSucceeds)
{
    for (int round = 0; round < 4; round++) {
        for (size_t i = 0; i < 64; i++) ASSERT_EQ(insert(item(i)), 0);
        for (size_t i = 0; i < 64; i++) ASSERT_EQ(lookup(item(i)), item(i));
        for (size_t i = 0; i < 64; i++) ASSERT_EQ(remove(item(i)), item(i));
        for (size_t i = 0; i < 64; i++) EXPECT_EQ(lookup(item(i)), nullptr);
    }
}

TEST_F(ConfigurableStoreTest, GrowthPreservesEveryItem)
{
    const size_t count = 5000;
    const size_t pfactor_before = store.hashtable.pfactor;

    for (size_t i = 0; i < count; i++) ASSERT_EQ(insert(item(i)), 0);

    EXPECT_GT(store.hashtable.pfactor, pfactor_before) << "the store never grew";
    EXPECT_LT(count, buckets());

    for (size_t i = 0; i < count; i++) EXPECT_EQ(lookup(item(i)), item(i));
    EXPECT_EQ(lookup(item(count)), nullptr);
}

TEST_F(ConfigurableStoreTest, RemovalsAcrossGrownRunsStayConsistent)
{
    const size_t count = 4096;

    for (size_t i = 0; i < count; i++) ASSERT_EQ(insert(item(i)), 0);
    for (size_t i = 0; i < count; i += 2) ASSERT_EQ(remove(item(i)), item(i));

    for (size_t i = 0; i < count; i++) {
        EXPECT_EQ(lookup(item(i)), (i % 2) ? item(i) : nullptr) << "index " << i;
    }

    for (size_t i = 0; i < count; i++) EXPECT_EQ(remove(item(i)), (i % 2) ? item(i) : nullptr);
    for (size_t i = 0; i < count; i++) EXPECT_EQ(lookup(item(i)), nullptr);
}

TEST_F(ConfigurableStoreTest, DuplicateInsertIsCallersResponsibility)
{
    /* The table does not reject duplicates: the API documents that the caller
     * enforces uniqueness.  Pin the consequence so a change is noticed. */
    ASSERT_EQ(insert(item(1)), 0);
    ASSERT_EQ(insert(item(1)), 0);

    EXPECT_EQ(lookup(item(1)), item(1));
    EXPECT_EQ(remove(item(1)), item(1));
    EXPECT_EQ(lookup(item(1)), item(1)) << "second copy is still present";
    EXPECT_EQ(remove(item(1)), item(1));
    EXPECT_EQ(lookup(item(1)), nullptr);
}

TEST_F(ConfigurableStoreTest, CollidingHashStillStoresEveryItem)
{
    /* Every key hashes to bucket 0, so the whole table is one run and it must
     * keep growing; correctness must not depend on the hash spreading. */
    store.hash_func = constant_hash;

    const size_t count = 512;
    for (size_t i = 0; i < count; i++) ASSERT_EQ(insert(item(i)), 0) << "index " << i;

    EXPECT_GT(store.hashtable.pfactor, 4) << "a colliding run must still grow the table";
    EXPECT_EQ(store.hashtable.count, count);

    for (size_t i = 0; i < count; i++) EXPECT_EQ(lookup(item(i)), item(i)) << "index " << i;

    for (size_t i = 0; i < count; i += 3) ASSERT_EQ(remove(item(i)), item(i)) << "index " << i;
    for (size_t i = 0; i < count; i++) EXPECT_EQ(lookup(item(i)), (i % 3) ? item(i) : nullptr) << "index " << i;
}

/* A caller that keys on a field rather than the pointer: the store hashes and
 * compares keylen bytes at key_offset inside the item, so two different objects
 * with the same id are the same key. */
struct KeyedEntry {
    uint64_t  id;
    char      padding[8];
    uint32_t  payload;
};

TEST_F(ConfigurableStoreTest, ContentKeyFindsByFieldNotPointer)
{
    store.keylen = sizeof(uint64_t);
    store.key_offset = offsetof(KeyedEntry, id);

    static KeyedEntry entries[64];
    for (size_t i = 0; i < 64; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].id = 1000 + i;
        entries[i].payload = (uint32_t)i;

        ASSERT_EQ(insert(&entries[i]), 0) << "index " << i;
    }

    for (size_t i = 0; i < 64; i++) {
        KeyedEntry probe;

        memset(&probe, 0, sizeof(probe));
        probe.id = 1000 + i;
        EXPECT_EQ(lookup(&probe), &entries[i]) << "index " << i;
    }

    KeyedEntry absent;

    memset(&absent, 0, sizeof(absent));
    absent.id = 999;
    EXPECT_EQ(lookup(&absent), nullptr);

    KeyedEntry removal;

    memset(&removal, 0, sizeof(removal));
    removal.id = 1042;
    EXPECT_EQ(remove(&removal), &entries[42]);
}

TEST_F(ConfigurableStoreTest, ContentKeyPaysNoAttentionToTheRestOfTheItem)
{
    store.keylen = sizeof(uint64_t);
    store.key_offset = offsetof(KeyedEntry, id);

    static KeyedEntry first, second;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.id = second.id = 7;
    first.payload = 1;
    second.payload = 2;

    ASSERT_EQ(insert(&first), 0);
    EXPECT_EQ(lookup(&second), &first) << "same key, different object";
}

TEST_F(ConfigurableStoreTest, RandomOperationsMatchModel)
{
    enum { PoolSize = 512, OpCount = 40000 };

    Rng rng(0xc0ffee);
    bool present[PoolSize] = {false};

    for (int op = 0; op < OpCount; op++) {
        const size_t idx = rng.below(PoolSize);
        void *ptr = item(idx);
        const unsigned action = (unsigned)rng.below(3);

        if (action == 0) {
            const bool was_present = present[idx];

            EXPECT_EQ(insert(ptr), 0);
            if (was_present) {
                /* the duplicate is inserted but must not hide the original */
                EXPECT_EQ(remove(ptr), ptr);
            }
            present[idx] = true;
        } else if (action == 1) {
            if (present[idx]) {
                EXPECT_EQ(remove(ptr), ptr) << "op " << op;
                present[idx] = false;
            } else {
                EXPECT_EQ(remove(ptr), nullptr) << "op " << op;
            }
        } else {
            EXPECT_EQ(lookup(ptr) != nullptr, present[idx]) << "op " << op;
        }
    }

    for (size_t i = 0; i < PoolSize; i++) {
        EXPECT_EQ(lookup(item(i)) != nullptr, present[i]) << "final index " << i;
    }
}

TEST_F(ConfigurableStoreTest, DeleteAllThenRebuild)
{
    const size_t count = 1024;

    for (size_t i = 0; i < count; i++) ASSERT_EQ(insert(item(i)), 0);
    for (size_t i = 0; i < count; i++) ASSERT_EQ(remove(item(i)), item(i));
    for (size_t i = 0; i < count; i++) ASSERT_EQ(lookup(item(i)), nullptr);

    for (size_t i = 0; i < count; i++) ASSERT_EQ(insert(item(i)), 0);
    for (size_t i = 0; i < count; i++) EXPECT_EQ(lookup(item(i)), item(i));
}

}  // namespace
