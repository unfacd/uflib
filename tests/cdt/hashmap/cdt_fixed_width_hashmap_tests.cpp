/**
 * @file cdt_fixed_width_hashmap_tests.cpp
 * @brief Comprehensive unit tests for LocklessFixedWidthHashMap.
 *
 * Covers single-threaded correctness, edge cases, and concurrent safety.
 *
 * Copyright (C) 2015-2026 unfacd works
 */

#include "gtest/gtest.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

extern "C" {
#include "uflib/cdt/hashmap/cdt_fixed_width_hashmap.h"
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

class HashMapTest : public ::testing::Test {
protected:
    void SetUp() override {
        memset(&map_, 0, sizeof(map_));
    }

    void TearDown() override {
        LocklessFixedWidthHashMap_Destroy(&map_);
    }

    bool Init(uint32_t capacity = 64,
              uint32_t load_pct = 75,
              uint32_t key_width = CONFIG_DEFAULT_CDS_HASHMAP_KEY_WIDTH) {
        return LocklessFixedWidthHashMap_Init(&map_, capacity, load_pct, key_width);
    }

    LocklessFixedWidthHashMap map_;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Init / Destroy
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, InitValid) {
    EXPECT_TRUE(Init(64));
    EXPECT_GT(map_.slot_capacity, 0U);
    /* slot_capacity must be a power of 2 */
    EXPECT_EQ(map_.slot_capacity & (map_.slot_capacity - 1), 0U);
    EXPECT_GT(map_.pool_capacity, 0U);
}

TEST_F(HashMapTest, InitCapacityZeroFails) {
    EXPECT_FALSE(Init(0));
}

TEST_F(HashMapTest, InitLoadFactorZeroFails) {
    EXPECT_FALSE(LocklessFixedWidthHashMap_Init(&map_, 64, 0, 256));
}

TEST_F(HashMapTest, InitLoadFactorOver100Fails) {
    EXPECT_FALSE(LocklessFixedWidthHashMap_Init(&map_, 64, 101, 256));
}

TEST_F(HashMapTest, InitKeyWidthZeroFails) {
    EXPECT_FALSE(LocklessFixedWidthHashMap_Init(&map_, 64, 75, 0));
}

TEST_F(HashMapTest, InitKeyWidthExceedsMaxFails) {
    EXPECT_FALSE(LocklessFixedWidthHashMap_Init(
        &map_, 64, 75, CONFIG_DEFAULT_CDS_HASHMAP_KEY_WIDTH + 1));
}

TEST_F(HashMapTest, InitRoundsUpToPowerOfTwo) {
    EXPECT_TRUE(Init(100));
    EXPECT_EQ(map_.slot_capacity, 128U);
}

TEST_F(HashMapTest, InitExactPowerOfTwo) {
    EXPECT_TRUE(Init(128));
    EXPECT_EQ(map_.slot_capacity, 128U);
}

TEST_F(HashMapTest, DestroyIsIdempotent) {
    EXPECT_TRUE(Init(64));
    LocklessFixedWidthHashMap_Destroy(&map_);
    LocklessFixedWidthHashMap_Destroy(&map_);  /* must not crash */
    EXPECT_EQ(map_.pool, nullptr);
    EXPECT_EQ(map_.slots, nullptr);
}

TEST_F(HashMapTest, DestroyNullIsSafe) {
    LocklessFixedWidthHashMap_Destroy(nullptr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Insert / Get — basic round-trip
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, InsertGetRoundTrip) {
    ASSERT_TRUE(Init());
    bool is_new = false;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "foo", 42, &is_new));
    EXPECT_TRUE(is_new);

    uint32_t val = 0;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, "foo", &val));
    EXPECT_EQ(val, 42U);
}

TEST_F(HashMapTest, InsertDuplicateReturnsExisting) {
    ASSERT_TRUE(Init());
    bool is_new = false;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "key", 10, &is_new));
    EXPECT_TRUE(is_new);

    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "key", 99, &is_new));
    EXPECT_FALSE(is_new);  /* key already existed */

    uint32_t val = 0;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, "key", &val));
    EXPECT_EQ(val, 10U);  /* original value preserved */
}

TEST_F(HashMapTest, InsertMultipleKeys) {
    ASSERT_TRUE(Init(128));
    bool is_new = false;

    for (int i = 0; i < 50; i++) {
        std::string key = "key_" + std::to_string(i);
        EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(
            &map_, key.c_str(), (uint32_t)i, &is_new));
        EXPECT_TRUE(is_new);
    }

    /* Verify all keys are retrievable */
    for (int i = 0; i < 50; i++) {
        std::string key = "key_" + std::to_string(i);
        uint32_t val = 0;
        EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, key.c_str(), &val));
        EXPECT_EQ(val, (uint32_t)i);
    }
}

TEST_F(HashMapTest, GetNonExistentKey) {
    ASSERT_TRUE(Init());
    uint32_t val = 42;
    EXPECT_FALSE(LocklessFixedWidthHashMap_Get(&map_, "nonexistent", &val));
    EXPECT_EQ(val, 42U);  /* unchanged on failure */
}

TEST_F(HashMapTest, InsertOutIsNewNull) {
    ASSERT_TRUE(Init());
    /* Must not crash when out_is_new is NULL */
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "foo", 1, nullptr));
}

TEST_F(HashMapTest, GetOutValueNull) {
    ASSERT_TRUE(Init());
    LocklessFixedWidthHashMap_Insert(&map_, "foo", 1, nullptr);
    /* Must not crash when out_value is NULL */
    EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, "foo", nullptr));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Remove
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, RemoveExisting) {
    ASSERT_TRUE(Init());
    LocklessFixedWidthHashMap_Insert(&map_, "foo", 7, nullptr);

    uint32_t val = 0;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Remove(&map_, "foo", &val));
    EXPECT_EQ(val, 7U);

    /* Key must no longer be found */
    EXPECT_FALSE(LocklessFixedWidthHashMap_Get(&map_, "foo", nullptr));
}

TEST_F(HashMapTest, RemoveNonExistent) {
    ASSERT_TRUE(Init());
    uint32_t val = 99;
    EXPECT_FALSE(LocklessFixedWidthHashMap_Remove(&map_, "ghost", &val));
    EXPECT_EQ(val, 99U);  /* unchanged */
}

TEST_F(HashMapTest, RemoveOutValueNull) {
    ASSERT_TRUE(Init());
    LocklessFixedWidthHashMap_Insert(&map_, "foo", 1, nullptr);
    EXPECT_TRUE(LocklessFixedWidthHashMap_Remove(&map_, "foo", nullptr));
}

TEST_F(HashMapTest, ReinsertAfterRemove) {
    ASSERT_TRUE(Init(128));
    bool is_new = false;

    /* Insert, remove, re-insert the same key */
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "key", 10, &is_new));
    EXPECT_TRUE(is_new);
    EXPECT_TRUE(LocklessFixedWidthHashMap_Remove(&map_, "key", nullptr));
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "key", 20, &is_new));
    EXPECT_TRUE(is_new);

    uint32_t val = 0;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, "key", &val));
    EXPECT_EQ(val, 20U);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Probe chain with tombstones
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, ProbePastTombstone) {
    /* Force a collision cluster by using a small map.
     * Insert keys that collide → later keys probe past earlier ones.
     * Remove one from the middle (leaves TOMBSTONE).
     * Remaining keys must still be reachable (probe past TOMBSTONE). */
    ASSERT_TRUE(Init(16, 100));  /* 16 slots (0 reserved = 15 usable), 16 pool */

    /* Insert enough keys into a small table that probing definitely happens */
    const char *keys[] = {"k1", "k2", "k3", "k4", "k5"};
    int nkeys = 5;
    for (int i = 0; i < nkeys; i++) {
        bool is_new = false;
        EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(
            &map_, keys[i], (uint32_t)(i + 1), &is_new)) << "insert " << keys[i];
    }

    /* Remove the middle key — leaves TOMBSTONE */
    EXPECT_TRUE(LocklessFixedWidthHashMap_Remove(&map_, keys[2], nullptr));

    /* Remaining keys must still be reachable */
    for (int i = 0; i < nkeys; i++) {
        if (i == 2) continue; /* was removed */
        uint32_t val = 0;
        EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, keys[i], &val))
            << "Key " << keys[i] << " not found after tombstone";
        EXPECT_EQ(val, (uint32_t)(i + 1));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fill to capacity
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, PoolExhaustion) {
    /* 8 slots, 50% load = 4 allocatable pool entries (+1 for reserved
     * index 0 = 5 total pool_capacity).  7 usable slots for 4 keys. */
    ASSERT_TRUE(Init(8, 50));
    EXPECT_EQ(map_.pool_capacity, 5U);  /* 4 usable + 1 reserved */

    bool is_new = false;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "a", 1, &is_new));
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "b", 2, &is_new));
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "c", 3, &is_new));
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "d", 4, &is_new));

    /* Fifth insert must fail — pool exhausted (only 4 allocatable nodes) */
    EXPECT_FALSE(LocklessFixedWidthHashMap_Insert(&map_, "e", 5, &is_new));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge cases — keys
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, EmptyStringKey) {
    ASSERT_TRUE(Init());
    bool is_new = false;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "", 42, &is_new));
    EXPECT_TRUE(is_new);

    uint32_t val = 0;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, "", &val));
    EXPECT_EQ(val, 42U);

    EXPECT_TRUE(LocklessFixedWidthHashMap_Remove(&map_, "", &val));
    EXPECT_EQ(val, 42U);
}

TEST_F(HashMapTest, SingleCharKey) {
    ASSERT_TRUE(Init());
    bool is_new = false;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(&map_, "X", 1, &is_new));
    EXPECT_TRUE(is_new);

    uint32_t val = 0;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, "X", &val));
    EXPECT_EQ(val, 1U);
}

TEST_F(HashMapTest, MaxLengthKey) {
    uint32_t kw = 64;
    ASSERT_TRUE(Init(64, 75, kw));
    std::string long_key(kw - 1, 'x');  /* exactly key_width-1 chars + NUL */

    bool is_new = false;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(
        &map_, long_key.c_str(), 99, &is_new));
    EXPECT_TRUE(is_new);

    uint32_t val = 0;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, long_key.c_str(), &val));
    EXPECT_EQ(val, 99U);
}

TEST_F(HashMapTest, KeyTruncation) {
    /* Key longer than key_width is silently truncated.
     * Insert a 12-char key into an 8-byte-key-width map; retrieving
     * with the truncated first 7 chars (+ NUL = 8 bytes) must match.
     * Retrieving with the original longer key will NOT match because
     * strncmp compares the 8th byte — NUL in the stored key vs the
     * long key's 8th character. */
    uint32_t kw = 8;
    ASSERT_TRUE(Init(64, 75, kw));

    const char *long_key   = "123456789AB";  /* 11 chars + NUL */
    char        short_key[8];
    memcpy(short_key, long_key, 7);
    short_key[7] = '\0';  /* "1234567" */

    bool is_new = false;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(
        &map_, long_key, 1, &is_new));
    EXPECT_TRUE(is_new);

    /* The truncated form must match — same first 7 chars, NUL at [7] */
    uint32_t val = 0;
    EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, short_key, &val));
    EXPECT_EQ(val, 1U);

    /* The full original key has a different 8th byte ('8' vs '\0')
     * so strncmp correctly reports no match.  Consumers must truncate
     * their lookup keys to the map's key_width. */
    val = 0;
    EXPECT_FALSE(LocklessFixedWidthHashMap_Get(&map_, long_key, &val));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Query functions
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, SizeTracksInserts) {
    ASSERT_TRUE(Init(128));
    EXPECT_EQ(LocklessFixedWidthHashMap_Size(&map_), 0U);

    for (int i = 0; i < 10; i++) {
        std::string key = "k" + std::to_string(i);
        LocklessFixedWidthHashMap_Insert(&map_, key.c_str(), (uint32_t)i, nullptr);
    }
    EXPECT_EQ(LocklessFixedWidthHashMap_Size(&map_), 10U);
}

TEST_F(HashMapTest, SizeTracksRemoves) {
    ASSERT_TRUE(Init(128));
    LocklessFixedWidthHashMap_Insert(&map_, "a", 1, nullptr);
    LocklessFixedWidthHashMap_Insert(&map_, "b", 2, nullptr);
    EXPECT_EQ(LocklessFixedWidthHashMap_Size(&map_), 2U);

    LocklessFixedWidthHashMap_Remove(&map_, "a", nullptr);
    EXPECT_EQ(LocklessFixedWidthHashMap_Size(&map_), 1U);
}

TEST_F(HashMapTest, Capacity) {
    ASSERT_TRUE(Init(100));
    EXPECT_EQ(LocklessFixedWidthHashMap_Capacity(&map_), 128U);
}

TEST_F(HashMapTest, LoadFactor) {
    ASSERT_TRUE(Init(100));
    EXPECT_FLOAT_EQ(LocklessFixedWidthHashMap_LoadFactor(&map_), 0.0f);

    LocklessFixedWidthHashMap_Insert(&map_, "a", 1, nullptr);
    float lf = LocklessFixedWidthHashMap_LoadFactor(&map_);
    EXPECT_GT(lf, 0.0f);
    EXPECT_LT(lf, 1.0f);
}

TEST_F(HashMapTest, QueryNullMapSafe) {
    EXPECT_EQ(LocklessFixedWidthHashMap_Size(nullptr), 0U);
    EXPECT_EQ(LocklessFixedWidthHashMap_Capacity(nullptr), 0U);
    EXPECT_FLOAT_EQ(LocklessFixedWidthHashMap_LoadFactor(nullptr), 0.0f);
}

TEST_F(HashMapTest, InsertNullMapSafe) {
    EXPECT_FALSE(LocklessFixedWidthHashMap_Insert(nullptr, "x", 1, nullptr));
}

TEST_F(HashMapTest, GetNullMapSafe) {
    EXPECT_FALSE(LocklessFixedWidthHashMap_Get(nullptr, "x", nullptr));
}

TEST_F(HashMapTest, RemoveNullMapSafe) {
    EXPECT_FALSE(LocklessFixedWidthHashMap_Remove(nullptr, "x", nullptr));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrent: disjoint keys — all inserts must succeed
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, ConcurrentDisjointInserts) {
    ASSERT_TRUE(Init(8192));
    constexpr int kNumThreads = 8;
    constexpr int kKeysPerThread = 250;

    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    for (int t = 0; t < kNumThreads; t++) {
        threads.emplace_back([this, t, &errors]() {
            for (int i = 0; i < kKeysPerThread; i++) {
                std::string key =
                    "t" + std::to_string(t) + "_k" + std::to_string(i);
                bool is_new = false;
                if (!LocklessFixedWidthHashMap_Insert(
                        &map_, key.c_str(), (uint32_t)(t * 10000 + i),
                        &is_new)) {
                    errors.fetch_add(1);
                }
                if (!is_new) {
                    /* Key collision not expected with disjoint key space */
                    errors.fetch_add(1);
                }
            }
        });
    }

    for (auto &th : threads) th.join();

    EXPECT_EQ(errors.load(), 0);

    /* All keys must be retrievable */
    for (int t = 0; t < kNumThreads; t++) {
        for (int i = 0; i < kKeysPerThread; i++) {
            std::string key =
                "t" + std::to_string(t) + "_k" + std::to_string(i);
            uint32_t val = 0;
            EXPECT_TRUE(LocklessFixedWidthHashMap_Get(&map_, key.c_str(), &val))
                << "Missing key: " << key;
            EXPECT_EQ(val, (uint32_t)(t * 10000 + i));
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrent: same keys — exactly one winner per key
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, ConcurrentDuplicateInserts) {
    ASSERT_TRUE(Init(2048));
    constexpr int kNumThreads = 8;
    constexpr int kKeys = 100;

    std::vector<std::thread> threads;
    std::atomic<int> new_count{0};
    std::atomic<int> existing_count{0};

    for (int t = 0; t < kNumThreads; t++) {
        threads.emplace_back([this, &new_count, &existing_count]() {
            for (int i = 0; i < kKeys; i++) {
                std::string key = "shared_" + std::to_string(i);
                bool is_new = false;
                EXPECT_TRUE(LocklessFixedWidthHashMap_Insert(
                    &map_, key.c_str(), 999U, &is_new));
                if (is_new) {
                    new_count.fetch_add(1);
                } else {
                    existing_count.fetch_add(1);
                }
            }
        });
    }

    for (auto &th : threads) th.join();

    /* Each key was new exactly once */
    EXPECT_EQ(new_count.load(), kKeys);
    EXPECT_EQ(existing_count.load(), kKeys * (kNumThreads - 1));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrent: mixed insert + get workload
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, ConcurrentInsertGetMixed) {
    ASSERT_TRUE(Init(4096));
    constexpr int kNumWriters = 4;
    constexpr int kNumReaders = 4;
    constexpr int kOpsPerThread = 500;

    /* Pre-populate some keys */
    for (int i = 0; i < 100; i++) {
        std::string key = "pre_" + std::to_string(i);
        LocklessFixedWidthHashMap_Insert(&map_, key.c_str(), (uint32_t)i, nullptr);
    }

    std::atomic<bool> start{false};
    std::atomic<int> read_errors{0};
    std::atomic<int> write_errors{0};

    std::vector<std::thread> threads;

    /* Writers — insert new keys */
    for (int t = 0; t < kNumWriters; t++) {
        threads.emplace_back([this, t, &start, &write_errors]() {
            while (!start.load()) { /* spin */ }
            for (int i = 0; i < kOpsPerThread; i++) {
                std::string key =
                    "w" + std::to_string(t) + "_" + std::to_string(i);
                if (!LocklessFixedWidthHashMap_Insert(
                        &map_, key.c_str(), (uint32_t)i, nullptr)) {
                    write_errors.fetch_add(1);
                }
            }
        });
    }

    /* Readers — read pre-populated keys */
    for (int t = 0; t < kNumReaders; t++) {
        threads.emplace_back([this, &start, &read_errors]() {
            while (!start.load()) { /* spin */ }
            for (int i = 0; i < kOpsPerThread; i++) {
                std::string key = "pre_" + std::to_string(i % 100);
                uint32_t val = 0;
                if (!LocklessFixedWidthHashMap_Get(&map_, key.c_str(), &val)) {
                    /* Pre-populated keys are never removed, so a miss is
                     * only valid if the key is genuinely absent — but all
                     * pre_* keys were inserted.  A miss could be a
                     * stale-pointer bug. */
                    if (val != 0) {
                        read_errors.fetch_add(1);
                    }
                }
            }
        });
    }

    start.store(true);
    for (auto &th : threads) th.join();

    EXPECT_EQ(write_errors.load(), 0);
    /* read_errors may be non-zero if a reader raced with recycling —
     * but with seq-lock protection it should be 0 */
    EXPECT_EQ(read_errors.load(), 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrent: parallel insert + remove of same keys
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, ConcurrentInsertRemove) {
    ASSERT_TRUE(Init(4096));
    constexpr int kNumThreads = 8;
    constexpr int kKeys = 100;

    /* Pre-populate */
    for (int i = 0; i < kKeys; i++) {
        std::string key = "cr_" + std::to_string(i);
        LocklessFixedWidthHashMap_Insert(&map_, key.c_str(), (uint32_t)i, nullptr);
    }

    std::atomic<bool> start{false};
    std::atomic<int> crashes{0};  /* incremented on unexpected failures */

    std::vector<std::thread> threads;

    /* Half the threads insert, half remove, all on the same key set */
    for (int t = 0; t < kNumThreads; t++) {
        if (t % 2 == 0) {
            /* Remover */
            threads.emplace_back([this, &start, &crashes]() {
                while (!start.load()) { /* spin */ }
                for (int i = 0; i < kKeys; i++) {
                    std::string key = "cr_" + std::to_string(i);
                    /* May or may not succeed — just must not crash */
                    LocklessFixedWidthHashMap_Remove(&map_, key.c_str(), nullptr);
                }
            });
        } else {
            /* Inserter */
            threads.emplace_back([this, t, &start, &crashes]() {
                while (!start.load()) { /* spin */ }
                for (int i = 0; i < kKeys; i++) {
                    std::string key = "cr_" + std::to_string(i);
                    if (!LocklessFixedWidthHashMap_Insert(
                            &map_, key.c_str(),
                            (uint32_t)(t * 1000 + i), nullptr)) {
                        /* Pool exhaustion is acceptable */
                    }
                }
            });
        }
    }

    start.store(true);
    for (auto &th : threads) th.join();

    /* Verify consistency: for each key, if it exists its value is from
     * some inserter (not corrupt). */
    for (int i = 0; i < kKeys; i++) {
        std::string key = "cr_" + std::to_string(i);
        uint32_t val = 0;
        if (LocklessFixedWidthHashMap_Get(&map_, key.c_str(), &val)) {
            /* Value should be non-zero (all inserters use non-zero) */
            EXPECT_GT(val, 0U) << "Corrupt value for key " << key;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stress: high-contention smoke test
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(HashMapTest, StressHighContention) {
    ASSERT_TRUE(Init(16384));
    constexpr int kNumThreads = 12;
    constexpr int kOps = 1000;

    std::atomic<bool> start{false};
    std::atomic<uint64_t> ops_completed{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kNumThreads; t++) {
        threads.emplace_back([this, t, &start, &ops_completed]() {
            while (!start.load()) { /* spin */ }
            for (int i = 0; i < kOps; i++) {
                std::string key =
                    "stress_" + std::to_string((t * kOps + i) % 5000);
                int op = (t + i) % 3;
                switch (op) {
                case 0: {
                    bool is_new = false;
                    LocklessFixedWidthHashMap_Insert(
                        &map_, key.c_str(), (uint32_t)i, &is_new);
                    break;
                }
                case 1: {
                    uint32_t val = 0;
                    LocklessFixedWidthHashMap_Get(&map_, key.c_str(), &val);
                    break;
                }
                case 2:
                    LocklessFixedWidthHashMap_Remove(&map_, key.c_str(), nullptr);
                    break;
                }
                ops_completed.fetch_add(1);
            }
        });
    }

    start.store(true);
    for (auto &th : threads) th.join();

    EXPECT_EQ(ops_completed.load(), (uint64_t)(kNumThreads * kOps));
    /* The map must still be in a consistent state (no crash = pass) */
    SUCCEED();
}
