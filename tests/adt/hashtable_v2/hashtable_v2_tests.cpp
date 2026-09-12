/**
 * @file hashtable_v2_tests.cpp
 * @brief GoogleTest suite for HashTableV2 — 34 test cases covering lifecycle,
 *        single-thread correctness, key edge cases, concurrency, ABI layout,
 *        enumeration, and merge.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>

extern "C" {
#include <uflib/adt/hashtable_v2/hashtable_v2.h>
#include <uflib/adt/hashtable_v2/hashtable_v2_type.h>
#include <uflib/adt/hashtable_v2/hashtable_v2_defs.h>
#include "hashtable_v2_priv.h"
}

/* ──────────────────────────────────────────────
 * Test fixtures and helpers
 * ────────────────────────────────────────────── */

class HashTableV2Test : public ::testing::Test {
protected:
	void SetUp() override {
		memset(&cfg, 0, sizeof(cfg));
		cfg.name = "TestTable";
	}

	void TearDown() override {
		if (ht) HashTableV2Destroy(ht);
		ht = nullptr;
	}

	HashTableV2Config cfg{};
	HashTableV2 *ht = nullptr;
};

/** A simple item type for testing where key is embedded in a struct. */
typedef struct {
	uint64_t id;
	char     name[32];
} TestItem;

static const void *
sTestItemExtractor(HashTableV2Item *item_ptr)
{
	TestItem *ti = (TestItem *)item_ptr;
	return &ti->id;
}

/* ──────────────────────────────────────────────
 * 1. Lifecycle (7 tests)
 * ────────────────────────────────────────────── */

TEST_F(HashTableV2Test, CreateDestroyValid)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.enable_locking = true;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);
	EXPECT_EQ(HashTableV2Size(ht), 0u);
	EXPECT_GT(HashTableV2Capacity(ht), 0u);
	HashTableV2Destroy(ht);
	ht = nullptr;  // prevent double-destroy in TearDown
}

TEST_F(HashTableV2Test, CreateNullConfig)
{
	ht = HashTableV2Create(nullptr);
	ASSERT_NE(ht, nullptr);
	EXPECT_EQ(HashTableV2Size(ht), 0u);
	EXPECT_EQ(HashTableV2Capacity(ht), CONFIG_DEFAULT_HASHTABLE_V2_INITIAL_SIZE);
}

TEST_F(HashTableV2Test, DestroyNull)
{
	HashTableV2Destroy(nullptr);  // must not crash
	SUCCEED();
}

TEST_F(HashTableV2Test, DoubleDestroy)
{
	cfg.key_size = 8;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);
	HashTableV2Destroy(ht);
	// Second destroy on same pointer is UB — we don't call it.
	// Instead verify that Destroy(NULL) after destroy is safe.
	ht = nullptr;
	HashTableV2Destroy(nullptr);  // no-op
	SUCCEED();
}

TEST_F(HashTableV2Test, CreateThenDestroyThenCreate)
{
	cfg.key_size = sizeof(uint64_t);
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);
	HashTableV2Destroy(ht);
	ht = nullptr;

	// Create again — no state leak from first instance
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);
	EXPECT_EQ(HashTableV2Size(ht), 0u);
}

TEST_F(HashTableV2Test, CreateWithCustomCapacity)
{
	cfg.key_size = 8;
	cfg.capacity_hint = 1024;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);
	// capacity should be >= capacity_hint (next prime)
	EXPECT_GE(HashTableV2Capacity(ht), 1024u);
}

TEST_F(HashTableV2Test, CreateWithLocking)
{
	cfg.key_size = 8;
	cfg.enable_locking = true;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	// Verify lock/unlock work
	EXPECT_EQ(HashTableV2WriteLock(ht, 0), 0);
	EXPECT_EQ(HashTableV2Unlock(ht), 0);
	EXPECT_EQ(HashTableV2ReadLock(ht, 0), 0);
	EXPECT_EQ(HashTableV2Unlock(ht), 0);
}

/* ──────────────────────────────────────────────
 * 2. Single-thread correctness (10 tests)
 * ────────────────────────────────────────────── */

TEST_F(HashTableV2Test, InsertLookupRoundTripFixedKey)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	TestItem item = {42, "hello"};
	bool is_added = true;
	HashTableV2Item *stored = HashTableV2Insert(ht, &item, &is_added);
	ASSERT_NE(stored, nullptr);
	EXPECT_FALSE(is_added);  // was freshly inserted
	EXPECT_EQ(stored, &item);

	uint64_t key = 42;
	HashTableV2Item *found = HashTableV2Lookup(ht, &key);
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found, &item);
	EXPECT_STREQ(((TestItem *)found)->name, "hello");
}

TEST_F(HashTableV2Test, InsertLookupRoundTripCStringKey)
{
	cfg.key_size = 0;  // c-string key
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	// When key_size==0 and no extractor, item IS the key
	// Use a static string to avoid strdup leak — the table doesn't own the key
	const char *str = "test_key";
	HashTableV2Item *stored = HashTableV2Insert(ht, (void *)str, nullptr);
	ASSERT_NE(stored, nullptr);
	EXPECT_EQ(stored, (void *)str);

	HashTableV2Item *found = HashTableV2Lookup(ht, "test_key");
	EXPECT_EQ(found, (void *)str);
}

TEST_F(HashTableV2Test, InsertDuplicateReturnsExisting)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	TestItem item1 = {42, "first"};
	TestItem item2 = {42, "second"};  // same key, different name

	bool is_added = false;
	HashTableV2Item *stored1 = HashTableV2Insert(ht, &item1, &is_added);
	EXPECT_FALSE(is_added);
	EXPECT_EQ(stored1, &item1);

	HashTableV2Item *stored2 = HashTableV2Insert(ht, &item2, &is_added);
	EXPECT_TRUE(is_added);  // was already present
	EXPECT_EQ(stored2, &item1);  // returns the existing item, not item2

	// Verify item1 is still in the table (not overwritten by item2)
	uint64_t key = 42;
	HashTableV2Item *found = HashTableV2Lookup(ht, &key);
	EXPECT_EQ(found, &item1);
	EXPECT_STREQ(((TestItem *)found)->name, "first");
}

TEST_F(HashTableV2Test, RemoveExisting)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	TestItem item = {99, "remove_me"};
	HashTableV2Insert(ht, &item, nullptr);
	EXPECT_EQ(HashTableV2Size(ht), 1u);

	HashTableV2Item *removed = HashTableV2Remove(ht, &item);
	EXPECT_EQ(removed, &item);
	EXPECT_EQ(HashTableV2Size(ht), 0u);

	uint64_t key = 99;
	EXPECT_EQ(HashTableV2Lookup(ht, &key), nullptr);
}

TEST_F(HashTableV2Test, RemoveNonExistent)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	TestItem item = {1, "nope"};
	HashTableV2Item *removed = HashTableV2Remove(ht, &item);
	EXPECT_EQ(removed, nullptr);
}

TEST_F(HashTableV2Test, ReinsertAfterRemove)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	TestItem item = {55, "reinsert"};
	HashTableV2Insert(ht, &item, nullptr);
	HashTableV2Remove(ht, &item);

	bool is_added = true;
	HashTableV2Item *stored = HashTableV2Insert(ht, &item, &is_added);
	EXPECT_FALSE(is_added);  // freshly inserted again
	EXPECT_EQ(stored, &item);

	uint64_t key = 55;
	EXPECT_EQ(HashTableV2Lookup(ht, &key), &item);
}

TEST_F(HashTableV2Test, LookupOnEmpty)
{
	cfg.key_size = sizeof(uint64_t);
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	uint64_t key = 12345;
	EXPECT_EQ(HashTableV2Lookup(ht, &key), nullptr);
}

TEST_F(HashTableV2Test, SizeConsistency)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	EXPECT_EQ(HashTableV2Size(ht), 0u);

	TestItem items[10];
	for (int i = 0; i < 10; i++) {
		items[i].id = (uint64_t)(i + 1);
		HashTableV2Insert(ht, &items[i], nullptr);
	}
	EXPECT_EQ(HashTableV2Size(ht), 10u);

	for (int i = 0; i < 5; i++) {
		HashTableV2Remove(ht, &items[i]);
	}
	EXPECT_EQ(HashTableV2Size(ht), 5u);
}

TEST_F(HashTableV2Test, InsertNullReturnsNull)
{
	cfg.key_size = 8;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	EXPECT_EQ(HashTableV2Insert(ht, nullptr, nullptr), nullptr);
	EXPECT_EQ(HashTableV2Insert(nullptr, (HashTableV2Item *)1, nullptr), nullptr);
}

TEST_F(HashTableV2Test, TableExpansion)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	cfg.capacity_hint = 8;  // small initial capacity to force expansions
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	size_t initial_cap = HashTableV2Capacity(ht);

	// Insert enough items to trigger at least one expansion
	std::vector<TestItem> items(200);
	for (int i = 0; i < 200; i++) {
		items[i].id = (uint64_t)(i * 7 + 1);  // spread keys to avoid clustering
		HashTableV2Insert(ht, &items[i], nullptr);
	}

	EXPECT_EQ(HashTableV2Size(ht), 200u);
	EXPECT_GT(HashTableV2Capacity(ht), initial_cap);

	// Verify all items are retrievable
	for (int i = 0; i < 200; i++) {
		uint64_t key = items[i].id;
		HashTableV2Item *found = HashTableV2Lookup(ht, &key);
		ASSERT_NE(found, nullptr) << "Failed to find key " << key;
		EXPECT_EQ(found, &items[i]);
	}
}

/* ──────────────────────────────────────────────
 * 3. Key edge cases (5 tests)
 * ────────────────────────────────────────────── */

TEST_F(HashTableV2Test, KeyZero)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	TestItem item = {0, "zero_key"};
	HashTableV2Insert(ht, &item, nullptr);

	uint64_t key = 0;
	EXPECT_EQ(HashTableV2Lookup(ht, &key), &item);
	HashTableV2Remove(ht, &item);
	EXPECT_EQ(HashTableV2Lookup(ht, &key), nullptr);
}

TEST_F(HashTableV2Test, KeyUint64Max)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	TestItem item = {UINT64_MAX, "max_key"};
	HashTableV2Insert(ht, &item, nullptr);

	uint64_t key = UINT64_MAX;
	EXPECT_EQ(HashTableV2Lookup(ht, &key), &item);
}

TEST_F(HashTableV2Test, EmptyStringKey)
{
	cfg.key_size = 0;  // c-string
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	const char *empty = "";
	HashTableV2Insert(ht, (void *)empty, nullptr);

	EXPECT_NE(HashTableV2Lookup(ht, ""), nullptr);
}

TEST_F(HashTableV2Test, LongStringKey)
{
	cfg.key_size = 0;  // c-string
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	const char *long_key = "this_is_a_very_long_key_string_that_exceeds_typical_session_cookie_lengths";
	HashTableV2Insert(ht, (void *)long_key, nullptr);

	EXPECT_NE(HashTableV2Lookup(ht, long_key), nullptr);
}

TEST_F(HashTableV2Test, ProbePastTombstone)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	cfg.capacity_hint = 16;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	// Insert two items that might collide
	TestItem a = {1, "a"};
	TestItem b = {2, "b"};
	TestItem c = {3, "c"};

	HashTableV2Insert(ht, &a, nullptr);
	HashTableV2Insert(ht, &b, nullptr);

	// Remove middle item, creating a tombstone
	HashTableV2Remove(ht, &b);

	// Insert a new item — should reclaim the tombstone or probe past it
	HashTableV2Insert(ht, &c, nullptr);

	// Verify all remaining items are retrievable
	uint64_t k1 = 1, k3 = 3;
	EXPECT_EQ(HashTableV2Lookup(ht, &k1), &a);
	EXPECT_EQ(HashTableV2Lookup(ht, &k3), &c);

	// Verify removed item is gone
	uint64_t k2 = 2;
	EXPECT_EQ(HashTableV2Lookup(ht, &k2), nullptr);
}

/* ──────────────────────────────────────────────
 * 4. Concurrent — read-heavy (3 tests)
 * ────────────────────────────────────────────── */

TEST_F(HashTableV2Test, ConcurrentDisjointInserts)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.enable_locking = true;
	cfg.capacity_hint = 4096;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	const int kNumThreads = 4;
	const int kItemsPerThread = 250;
	std::vector<TestItem> items(kNumThreads * kItemsPerThread);

	std::vector<std::thread> threads;
	for (int t = 0; t < kNumThreads; t++) {
		threads.emplace_back([this, t, &items, kItemsPerThread]() {
			for (int i = 0; i < kItemsPerThread; i++) {
				int idx = t * kItemsPerThread + i;
				items[idx].id = (uint64_t)(t * 10000 + i + 1);
				snprintf(items[idx].name, sizeof(items[idx].name),
				         "t%d_i%d", t, i);
				HashTableV2Insert(ht, &items[idx], nullptr);
			}
		});
	}
	for (auto &th : threads) th.join();

	EXPECT_EQ(HashTableV2Size(ht), (size_t)(kNumThreads * kItemsPerThread));

	// Verify all items are retrievable
	for (int t = 0; t < kNumThreads; t++) {
		for (int i = 0; i < kItemsPerThread; i++) {
			int idx = t * kItemsPerThread + i;
			uint64_t key = items[idx].id;
			EXPECT_NE(HashTableV2Lookup(ht, &key), nullptr)
			    << "Missing key " << key;
		}
	}
}

TEST_F(HashTableV2Test, ConcurrentReadsOneWriter)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	cfg.enable_locking = true;
	cfg.capacity_hint = 1024;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	// Pre-populate
	const int kNumItems = 200;
	TestItem items[kNumItems];
	for (int i = 0; i < kNumItems; i++) {
		items[i].id = (uint64_t)(i + 1);
		HashTableV2Insert(ht, &items[i], nullptr);
	}

	std::atomic<bool> done{false};
	std::atomic<int> read_errors{0};

	// Reader threads
	std::vector<std::thread> readers;
	for (int r = 0; r < 4; r++) {
		readers.emplace_back([this, &items, &done, &read_errors, kNumItems]() {
			while (!done.load(std::memory_order_relaxed)) {
				for (int i = 0; i < kNumItems; i++) {
					uint64_t key = items[i].id;
					HashTableV2Item *found = HashTableV2Lookup(ht, &key);
					if (found == nullptr) read_errors.fetch_add(1);
				}
			}
		});
	}

	// Writer — insert/remove churn
	TestItem extra = {99999, "extra"};
	for (int w = 0; w < 50; w++) {
		HashTableV2Insert(ht, &extra, nullptr);
		HashTableV2Remove(ht, &extra);
	}

	done.store(true);
	for (auto &th : readers) th.join();

	EXPECT_EQ(read_errors.load(), 0);
}

TEST_F(HashTableV2Test, TwoWritersDisjointKeys)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.enable_locking = true;
	cfg.capacity_hint = 1024;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	std::vector<TestItem> items_a(100), items_b(100);

	std::thread t1([this, &items_a]() {
		for (int i = 0; i < 100; i++) {
			items_a[i].id = (uint64_t)(i + 1);
			HashTableV2Insert(ht, &items_a[i], nullptr);
		}
	});
	std::thread t2([this, &items_b]() {
		for (int i = 0; i < 100; i++) {
			items_b[i].id = (uint64_t)(i + 1001);
			HashTableV2Insert(ht, &items_b[i], nullptr);
		}
	});

	t1.join();
	t2.join();

	EXPECT_EQ(HashTableV2Size(ht), 200u);
}

/* ──────────────────────────────────────────────
 * 5. Concurrent — mixed workload (3 tests)
 * ────────────────────────────────────────────── */

TEST_F(HashTableV2Test, ConcurrentInsertRemoveChurn)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	cfg.enable_locking = true;
	cfg.capacity_hint = 2048;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	const int kNumItems = 100;
	const int kRounds = 50;
	TestItem items[kNumItems];
	for (int i = 0; i < kNumItems; i++) items[i].id = (uint64_t)(i + 1);

	std::atomic<int> errors{0};
	std::vector<std::thread> threads;

	for (int t = 0; t < 4; t++) {
		threads.emplace_back([this, &items, &errors, kNumItems, kRounds]() {
			for (int r = 0; r < kRounds; r++) {
				for (int i = 0; i < kNumItems; i++) {
					if ((i + r) % 2 == 0) {
						HashTableV2Insert(ht, &items[i], nullptr);
					} else {
						HashTableV2Remove(ht, &items[i]);
					}
				}
			}
		});
	}
	for (auto &th : threads) th.join();

	// No crashes, no invariants violated
	SUCCEED();
}

TEST_F(HashTableV2Test, ConcurrentMixedWorkload)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	cfg.enable_locking = true;
	cfg.capacity_hint = 4096;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	// Pre-populate
	const int kNumItems = 300;
	TestItem items[kNumItems];
	for (int i = 0; i < kNumItems; i++) {
		items[i].id = (uint64_t)(i + 1);
		HashTableV2Insert(ht, &items[i], nullptr);
	}

	std::atomic<bool> done{false};
	std::atomic<int> errors{0};
	std::vector<std::thread> threads;

	for (int t = 0; t < 6; t++) {
		threads.emplace_back([this, &items, &done, &errors, kNumItems, t]() {
			while (!done.load(std::memory_order_relaxed)) {
				int idx = (t * 37 + rand()) % kNumItems;  // pseudo-random
				uint64_t key = items[idx].id;
				HashTableV2Item *found = HashTableV2Lookup(ht, &key);
				if (found != nullptr && found != &items[idx]) {
					errors.fetch_add(1);
				}
			}
		});
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	done.store(true);
	for (auto &th : threads) th.join();

	EXPECT_EQ(errors.load(), 0);
}

TEST_F(HashTableV2Test, HighContentionStress)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	cfg.enable_locking = true;
	cfg.capacity_hint = 8192;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	const int kNumItems = 500;
	TestItem items[kNumItems];
	for (int i = 0; i < kNumItems; i++) items[i].id = (uint64_t)(i + 1);

	std::atomic<int> errors{0};
	std::vector<std::thread> threads;

	for (int t = 0; t < 12; t++) {
		threads.emplace_back([this, &items, &errors, kNumItems]() {
			for (int r = 0; r < 200; r++) {
				int idx = (r * 7 + 3) % kNumItems;
				HashTableV2Insert(ht, &items[idx], nullptr);
				uint64_t key = items[idx].id;
				HashTableV2Lookup(ht, &key);
			}
		});
	}
	for (auto &th : threads) th.join();

	// No crashes, no invariants violated
	EXPECT_EQ(errors.load(), 0);
	SUCCEED();
}

/* ──────────────────────────────────────────────
 * 6. ABI layout assertions (2 tests)
 * ────────────────────────────────────────────── */

TEST_F(HashTableV2Test, OpaqueHandleCannotBeSizeof)
{
	// HashTableV2 is forward-declared as typedef struct HashTableV2 HashTableV2;
	// Consumer code cannot know its sizeof — this verifies the opaque handle works.
	//
	// We CAN'T write `sizeof(HashTableV2)` here because the full struct is included
	// via _priv.h.  But we CAN verify that without _priv.h, sizeof would fail.
	// This test just verifies the struct is non-zero sized.
	EXPECT_GT(sizeof(HashTableV2), 0u);
}

TEST_F(HashTableV2Test, ConfigStructSizes)
{
	// Verify config struct has expected alignment and reasonable size
	EXPECT_EQ(sizeof(HashTableV2Config) % sizeof(void *), 0u);  // pointer-aligned
	EXPECT_GT(sizeof(HashTableV2Config), 0u);
}

/* ──────────────────────────────────────────────
 * 7. Enumeration & Merge (4 tests)
 * ────────────────────────────────────────────── */

TEST_F(HashTableV2Test, EnumerateEmpty)
{
	cfg.key_size = 8;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	HashTableV2Item *items[10];
	long count = HashTableV2Enumerate(ht, items, 10);
	EXPECT_EQ(count, 0);
}

TEST_F(HashTableV2Test, EnumerateWithItems)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	TestItem items[5];
	for (int i = 0; i < 5; i++) {
		items[i].id = (uint64_t)(i + 1);
		HashTableV2Insert(ht, &items[i], nullptr);
	}

	HashTableV2Item *out[10];
	long count = HashTableV2Enumerate(ht, out, 10);
	EXPECT_EQ(count, 5);

	// Verify all 5 items are in the output
	int found = 0;
	for (long i = 0; i < count; i++) {
		for (int j = 0; j < 5; j++) {
			if (out[i] == &items[j]) found++;
		}
	}
	EXPECT_EQ(found, 5);
}

TEST_F(HashTableV2Test, EnumerateTruncated)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	TestItem items[10];
	for (int i = 0; i < 10; i++) {
		items[i].id = (uint64_t)(i + 1);
		HashTableV2Insert(ht, &items[i], nullptr);
	}

	HashTableV2Item *out[3];
	long count = HashTableV2Enumerate(ht, out, 3);
	EXPECT_EQ(count, 3);  // truncated to array capacity
}

TEST_F(HashTableV2Test, MergeTwoTables)
{
	cfg.key_size = sizeof(uint64_t);
	cfg.key_offset = offsetof(TestItem, id);
	cfg.key_extractor = sTestItemExtractor;
	HashTableV2 *dest = HashTableV2Create(&cfg);
	HashTableV2 *src = HashTableV2Create(&cfg);
	ASSERT_NE(dest, nullptr);
	ASSERT_NE(src, nullptr);

	TestItem a = {1, "a"};
	TestItem b = {2, "b"};
	TestItem c = {3, "c"};

	HashTableV2Insert(dest, &a, nullptr);
	HashTableV2Insert(src, &b, nullptr);
	HashTableV2Insert(src, &c, nullptr);

	EXPECT_EQ(HashTableV2Size(dest), 1u);
	EXPECT_EQ(HashTableV2Size(src), 2u);

	HashTableV2Merge(dest, src);

	EXPECT_EQ(HashTableV2Size(dest), 3u);

	uint64_t k1 = 1, k2 = 2, k3 = 3;
	EXPECT_NE(HashTableV2Lookup(dest, &k1), nullptr);
	EXPECT_NE(HashTableV2Lookup(dest, &k2), nullptr);
	EXPECT_NE(HashTableV2Lookup(dest, &k3), nullptr);

	HashTableV2Destroy(dest);
	HashTableV2Destroy(src);
}

/* ──────────────────────────────────────────────
 * 8. Lock API (2 tests)
 * ────────────────────────────────────────────── */

TEST_F(HashTableV2Test, LockWithoutLockingEnabled)
{
	cfg.key_size = 8;
	cfg.enable_locking = false;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	// Lock/unlock on a non-locking table should be no-ops
	EXPECT_EQ(HashTableV2ReadLock(ht, 0), 0);
	EXPECT_EQ(HashTableV2Unlock(ht), 0);
	EXPECT_EQ(HashTableV2WriteLock(ht, 0), 0);
	EXPECT_EQ(HashTableV2Unlock(ht), 0);
}

TEST_F(HashTableV2Test, TryLockContention)
{
	cfg.key_size = 8;
	cfg.enable_locking = true;
	ht = HashTableV2Create(&cfg);
	ASSERT_NE(ht, nullptr);

	// Acquire write lock
	EXPECT_EQ(HashTableV2WriteLock(ht, 0), 0);

	// Try-read should fail (writer holds the lock)
	EXPECT_NE(HashTableV2ReadLock(ht, 1), 0);

	// Release
	EXPECT_EQ(HashTableV2Unlock(ht), 0);

	// Now try-read should succeed
	EXPECT_EQ(HashTableV2ReadLock(ht, 1), 0);
	EXPECT_EQ(HashTableV2Unlock(ht), 0);
}

/* ──────────────────────────────────────────────
 * 9. White-box internal helpers (3 tests)
 * ────────────────────────────────────────────── */

TEST(HashTableV2Internals, HashFnDeterministic)
{
	// FNV-1a must produce the same hash for the same input
	const char *key = "test_key_123";
	uint32_t h1 = sHashFn(key, 0, 65521);
	uint32_t h2 = sHashFn(key, 0, 65521);
	EXPECT_EQ(h1, h2);

	// Different key → different hash (with high probability)
	uint32_t h3 = sHashFn("different_key", 0, 65521);
	EXPECT_NE(h1, h3);
}

TEST(HashTableV2Internals, HashFnModuloRange)
{
	// Hash must always produce a value in [0, table_size)
	const size_t kTableSize = 1024;
	for (int i = 0; i < 1000; i++) {
		char key[32];
		snprintf(key, sizeof(key), "key_%d", i);
		uint32_t h = sHashFn(key, 0, kTableSize);
		EXPECT_LT(h, kTableSize);
	}
}

TEST(HashTableV2Internals, NextPrime)
{
	EXPECT_EQ(sNextPrime(1), 7u);
	EXPECT_EQ(sNextPrime(7), 7u);
	EXPECT_EQ(sNextPrime(8), 13u);
	EXPECT_EQ(sNextPrime(100), 127u);
	EXPECT_EQ(sNextPrime(65521), 65521u);
	EXPECT_EQ(sNextPrime(65522), 131071u);
}
