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

// adt_minheap_tests.cpp
// Hardened gtest suite for the single-threaded binary min-heap.
// Covers: lifecycle, NULL handles, empty ops, sort property, wide (any
// magnitude) comparators, duplicates, growth/reserve/clear, model-checked
// mixed ops, both key modes, mode-mismatch rejection, the capacity-overflow
// guard, and the i64 foreach round-trip.

#include "gtest/gtest.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

extern "C" {
#include <uflib/adt/adt_minheap.h>
}

namespace {

int cmp_int(const void *a, const void *b)
{
    int va = *static_cast<const int *>(a);
    int vb = *static_cast<const int *>(b);
    return (va > vb) - (va < vb);
}

// Returns -2 / 0 / +2 — deliberately NOT -1 / 0 / +1.  The pre-2026 heap
// sifted down only on `cmp == 1`; this regresses that defect.
int cmp_wide(const void *a, const void *b)
{
    int va = *static_cast<const int *>(a);
    int vb = *static_cast<const int *>(b);
    if (va < vb) return -2;
    if (va > vb) return 2;
    return 0;
}

struct Acc { std::vector<int> keys; };

void collect(void *k, void *, void *ctx)
{
    static_cast<Acc *>(ctx)->keys.push_back(*static_cast<int *>(k));
}

struct AccI64 { std::vector<int64_t> keys; };

void collect_i64(void *k, void *, void *ctx)
{
    // i64 keys arrive re-interpreted as void*; recover the integer.
    static_cast<AccI64 *>(ctx)->keys.push_back(static_cast<int64_t>(
        reinterpret_cast<intptr_t>(k)));
}

class MinHeapTest : public ::testing::Test {
protected:
    MinHeap *h = nullptr;
    void TearDown() override { MinHeapDestroy(h); h = nullptr; }
};

// ── Lifecycle ──────────────────────────────────────────────────────────

TEST_F(MinHeapTest, CreateDefaultAndNullComparator)
{
    h = MinHeapCreate(0, nullptr);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(MinHeapSize(h), 0);
    EXPECT_GE(MinHeapCapacity(h), 16);
    EXPECT_GT(MinHeapPageSize(), 0);
    EXPECT_GT(MinHeapEntriesPerPage(), 0);
}

TEST_F(MinHeapTest, DefaultComparatorIsIntKeys)
{
    h = MinHeapCreate(0, nullptr);  // NULL comparator → MinHeapCompareIntKeys
    int keys[] = {9, 1, 5, 3, 7};
    for (int &k : keys)
        ASSERT_EQ(MinHeapInsert(h, &k, &k), MINHEAP_OK);
    int prev = INT_MIN;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        void *k = nullptr;
        ASSERT_EQ(MinHeapDelmin(h, &k, nullptr), 1);
        EXPECT_GE(*static_cast<int *>(k), prev);
        prev = *static_cast<int *>(k);
    }
}

TEST_F(MinHeapTest, CreateRejectsNegativeCapacity)
{
    EXPECT_EQ(MinHeapCreate(-1, cmp_int), nullptr);
    EXPECT_EQ(MinHeapCreateI64(-1), nullptr);
}

TEST_F(MinHeapTest, NullHandleIsSafe)
{
    EXPECT_EQ(MinHeapSize(nullptr), 0);
    EXPECT_EQ(MinHeapCapacity(nullptr), 0);
    void *k = reinterpret_cast<void *>(1), *v = reinterpret_cast<void *>(2);
    EXPECT_EQ(MinHeapMin(nullptr, &k, &v), 0);
    EXPECT_EQ(MinHeapPeek(nullptr, &k, &v), 0);
    EXPECT_EQ(MinHeapDelmin(nullptr, &k, &v), 0);
    EXPECT_EQ(MinHeapPop(nullptr, &v), 0);
    EXPECT_EQ(MinHeapInsert(nullptr, &k, &v), MINHEAP_ERR_INVAL);
    EXPECT_EQ(MinHeapInsertI64(nullptr, 1, nullptr), MINHEAP_ERR_INVAL);
    EXPECT_EQ(MinHeapReserve(nullptr, 8), MINHEAP_ERR_INVAL);
    EXPECT_EQ(MinHeapMinI64(nullptr, nullptr, nullptr), 0);
    EXPECT_EQ(MinHeapDelminI64(nullptr, nullptr, nullptr), 0);
    EXPECT_EQ(MinHeapForeach(nullptr, nullptr, nullptr), 0u);
    MinHeapClear(nullptr);
    MinHeapDestroy(nullptr);
}

TEST_F(MinHeapTest, EmptyMinDelminPop)
{
    h = MinHeapCreate(8, cmp_int);
    void *k = nullptr, *v = nullptr;
    EXPECT_EQ(MinHeapMin(h, &k, &v), 0);
    EXPECT_EQ(MinHeapPeek(h, &k, &v), 0);
    EXPECT_EQ(MinHeapDelmin(h, &k, &v), 0);
    EXPECT_EQ(MinHeapPop(h, &v), 0);
}

TEST_F(MinHeapTest, DestroyIsIdempotentSafe)
{
    h = MinHeapCreate(4, cmp_int);
    MinHeapDestroy(h);
    h = nullptr;  // already freed; TearDown won't double-free
    MinHeapDestroy(nullptr);
}

// ── Insert / peek / remove, optional out-params ────────────────────────

TEST_F(MinHeapTest, InsertMinDelminOptionalOutParams)
{
    h = MinHeapCreate(8, cmp_int);
    int key = 42, val = 7;
    ASSERT_EQ(MinHeapInsert(h, &key, &val), MINHEAP_OK);
    EXPECT_EQ(MinHeapSize(h), 1);
    ASSERT_EQ(MinHeapMin(h, nullptr, nullptr), 1);
    void *v = nullptr;
    ASSERT_EQ(MinHeapPeek(h, nullptr, &v), 1);
    EXPECT_EQ(v, &val);
    ASSERT_EQ(MinHeapDelmin(h, nullptr, nullptr), 1);
    EXPECT_EQ(MinHeapSize(h), 0);
}

TEST_F(MinHeapTest, ExtractIsNonDecreasing)
{
    h = MinHeapCreate(16, cmp_int);
    int keys[] = {9, 1, 5, 3, 7, 2, 8, 0, 4, 6};
    for (int &k : keys)
        ASSERT_EQ(MinHeapInsert(h, &k, &k), MINHEAP_OK);
    int prev = INT_MIN;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        void *k = nullptr;
        ASSERT_EQ(MinHeapDelmin(h, &k, nullptr), 1);
        EXPECT_GE(*static_cast<int *>(k), prev);
        prev = *static_cast<int *>(k);
    }
    EXPECT_EQ(MinHeapSize(h), 0);
}

// ── Comparator contract ────────────────────────────────────────────────

TEST_F(MinHeapTest, WideComparatorSorts)
{
    h = MinHeapCreate(8, cmp_wide);
    int keys[] = {3, 1, 4, 2};
    for (int &k : keys)
        ASSERT_EQ(MinHeapInsert(h, &k, &k), MINHEAP_OK);
    const int expect[] = {1, 2, 3, 4};
    for (int e : expect) {
        void *k = nullptr;
        ASSERT_EQ(MinHeapDelmin(h, &k, nullptr), 1);
        EXPECT_EQ(*static_cast<int *>(k), e);
    }
}

TEST_F(MinHeapTest, WideComparatorLargeRandom)
{
    h = MinHeapCreate(8, cmp_wide);
    std::vector<int> keys(5000);
    std::mt19937 rng(42);
    for (int &k : keys) {
        k = static_cast<int>(rng() % 100000);
        ASSERT_EQ(MinHeapInsert(h, &k, &k), MINHEAP_OK);
    }
    // Extract everything, then compare against the sorted multiset.  Do NOT
    // sort `keys` while the heap still references its elements by pointer —
    // that mutates the values the comparator dereferences mid-flight (the
    // "keys must remain valid for the entry's lifetime" contract).
    std::vector<int> got;
    got.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        void *k = nullptr;
        ASSERT_EQ(MinHeapDelmin(h, &k, nullptr), 1);
        got.push_back(*static_cast<int *>(k));
    }
    std::vector<int> expect = keys;
    std::sort(expect.begin(), expect.end());
    EXPECT_EQ(got, expect);
}

TEST_F(MinHeapTest, DuplicatesAllowed)
{
    h = MinHeapCreate(4, cmp_int);
    int a = 5, b = 5, c = 5;
    ASSERT_EQ(MinHeapInsert(h, &a, &a), MINHEAP_OK);
    ASSERT_EQ(MinHeapInsert(h, &b, &b), MINHEAP_OK);
    ASSERT_EQ(MinHeapInsert(h, &c, &c), MINHEAP_OK);
    for (int i = 0; i < 3; ++i) {
        void *k = nullptr;
        ASSERT_EQ(MinHeapPop(h, &k), 1);
        EXPECT_EQ(*static_cast<int *>(k), 5);
    }
    EXPECT_EQ(MinHeapSize(h), 0);
}

TEST_F(MinHeapTest, CompareLongLongDereferences)
{
    long long a = 10, b = 99;
    EXPECT_LT(MinHeapCompareLongLongKeys(&a, &b), 0);
    EXPECT_GT(MinHeapCompareLongLongKeys(&b, &a), 0);
    EXPECT_EQ(MinHeapCompareLongLongKeys(&a, &a), 0);
}

TEST_F(MinHeapTest, ComparePtrAsInteger)
{
    void *small = reinterpret_cast<void *>(static_cast<uintptr_t>(10));
    void *large = reinterpret_cast<void *>(static_cast<uintptr_t>(99));
    EXPECT_LT(MinHeapComparePtrAsInteger(small, large), 0);
    EXPECT_GT(MinHeapComparePtrAsInteger(large, small), 0);
    EXPECT_EQ(MinHeapComparePtrAsInteger(small, small), 0);
}

// ── Growth / reserve / clear ───────────────────────────────────────────

TEST_F(MinHeapTest, ReserveAndGrowth)
{
    h = MinHeapCreate(1, cmp_int);
    ASSERT_EQ(MinHeapReserve(h, 1000), MINHEAP_OK);
    EXPECT_GE(MinHeapCapacity(h), 1000);
    std::vector<int> keys(2000);
    for (int i = 0; i < 2000; ++i) {
        keys[i] = 2000 - i;
        ASSERT_EQ(MinHeapInsert(h, &keys[i], &keys[i]), MINHEAP_OK);
    }
    EXPECT_EQ(MinHeapSize(h), 2000);
    int prev = INT_MIN;
    void *k = nullptr;
    while (MinHeapDelmin(h, &k, nullptr)) {
        EXPECT_GE(*static_cast<int *>(k), prev);
        prev = *static_cast<int *>(k);
    }
    EXPECT_EQ(MinHeapSize(h), 0);
}

TEST_F(MinHeapTest, ReserveRejectsNegative)
{
    h = MinHeapCreate(4, cmp_int);
    EXPECT_EQ(MinHeapReserve(h, -1), MINHEAP_ERR_INVAL);
}

// Regression for the sGrowTo signed-overflow guard: a reserve request past
// the internal capacity ceiling must return MINHEAP_ERR_OOM, not overflow or
// hang.  (Pre-fix, `cap *= 2` wrapped past INT_MAX and looped forever.)
TEST_F(MinHeapTest, ReserveHugeReturnsOOM)
{
    h = MinHeapCreate(4, cmp_int);
    EXPECT_EQ(MinHeapReserve(h, INT_MAX), MINHEAP_ERR_OOM);
    EXPECT_EQ(MinHeapReserve(h, 2000000000), MINHEAP_ERR_OOM);
    // Heap must remain usable after a rejected reserve.
    int k = 1;
    EXPECT_EQ(MinHeapInsert(h, &k, &k), MINHEAP_OK);
    EXPECT_EQ(MinHeapSize(h), 1);
}

TEST_F(MinHeapTest, ForeachAndClearReuse)
{
    h = MinHeapCreate(4, cmp_int);
    int keys[] = {4, 1, 3, 2};
    for (int &k : keys)
        ASSERT_EQ(MinHeapInsert(h, &k, &k), MINHEAP_OK);
    Acc acc;
    EXPECT_EQ(MinHeapForeach(h, collect, &acc), 4u);
    std::sort(acc.keys.begin(), acc.keys.end());
    EXPECT_EQ(acc.keys, (std::vector<int>{1, 2, 3, 4}));
    int cap = MinHeapCapacity(h);
    MinHeapClear(h);
    EXPECT_EQ(MinHeapSize(h), 0);
    EXPECT_EQ(MinHeapCapacity(h), cap);  // allocation retained
    ASSERT_EQ(MinHeapInsert(h, &keys[0], &keys[0]), MINHEAP_OK);
    EXPECT_EQ(MinHeapSize(h), 1);
}

TEST_F(MinHeapTest, ForeachNullAndEmptyReturnsZero)
{
    h = MinHeapCreate(4, cmp_int);
    EXPECT_EQ(MinHeapForeach(h, collect, nullptr), 0u);  // empty
    EXPECT_EQ(MinHeapForeach(h, nullptr, nullptr), 0u);  // NULL callback
}

// ── Model-checked mixed ops ────────────────────────────────────────────

TEST_F(MinHeapTest, MixedOpsMatchModel)
{
    h = MinHeapCreate(4, cmp_int);
    std::vector<int> live, storage;
    storage.reserve(256);
    std::mt19937 rng(12345);
    for (int step = 0; step < 300; ++step) {
        bool insert = live.empty() || (rng() % 3 != 0);
        if (insert) {
            storage.push_back(static_cast<int>(rng() % 1000));
            ASSERT_EQ(MinHeapInsert(h, &storage.back(), &storage.back()), MINHEAP_OK);
            live.push_back(storage.back());
        } else {
            void *k = nullptr;
            ASSERT_EQ(MinHeapDelmin(h, &k, nullptr), 1);
            auto it = std::min_element(live.begin(), live.end());
            EXPECT_EQ(*static_cast<int *>(k), *it);
            live.erase(it);
        }
        EXPECT_EQ(MinHeapSize(h), static_cast<int>(live.size()));
        if (!live.empty()) {
            void *k = nullptr;
            ASSERT_EQ(MinHeapMin(h, &k, nullptr), 1);
            EXPECT_EQ(*static_cast<int *>(k), *std::min_element(live.begin(), live.end()));
        }
    }
}

// ── i64 path ───────────────────────────────────────────────────────────

TEST_F(MinHeapTest, I64Path)
{
    h = MinHeapCreateI64(4);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(MinHeapInsert(h, nullptr, nullptr), MINHEAP_ERR_INVAL);  // ptr op on i64 heap
    ASSERT_EQ(MinHeapInsertI64(h, 9, reinterpret_cast<void *>(1)), MINHEAP_OK);
    ASSERT_EQ(MinHeapInsertI64(h, 1, reinterpret_cast<void *>(2)), MINHEAP_OK);
    ASSERT_EQ(MinHeapInsertI64(h, 5, reinterpret_cast<void *>(3)), MINHEAP_OK);
    int64_t k = 0;
    void *v = nullptr;
    ASSERT_EQ(MinHeapMinI64(h, &k, &v), 1);
    EXPECT_EQ(k, 1);
    EXPECT_EQ(v, reinterpret_cast<void *>(2));
    const int64_t expect[] = {1, 5, 9};
    for (int64_t e : expect) {
        ASSERT_EQ(MinHeapDelminI64(h, &k, nullptr), 1);
        EXPECT_EQ(k, e);
    }
    EXPECT_EQ(MinHeapSize(h), 0);
}

TEST_F(MinHeapTest, I64ManyAndPop)
{
    h = MinHeapCreateI64(2);
    const int n = 1500;
    for (int i = 0; i < n; ++i)
        ASSERT_EQ(MinHeapInsertI64(h, n - i, nullptr), MINHEAP_OK);
    int64_t prev = INT64_MIN;
    for (int i = 0; i < n; ++i) {
        int64_t k = 0;
        ASSERT_EQ(MinHeapDelminI64(h, &k, nullptr), 1);
        EXPECT_GE(k, prev);
        prev = k;
    }
}

TEST_F(MinHeapTest, I64PopReturnsValue)
{
    h = MinHeapCreateI64(4);
    ASSERT_EQ(MinHeapInsertI64(h, 5, reinterpret_cast<void *>(11)), MINHEAP_OK);
    ASSERT_EQ(MinHeapInsertI64(h, 3, reinterpret_cast<void *>(22)), MINHEAP_OK);
    void *v = nullptr;
    ASSERT_EQ(MinHeapPop(h, &v), 1);
    EXPECT_EQ(v, reinterpret_cast<void *>(22));  // min key 3
}

TEST_F(MinHeapTest, I64OptionalOutParamsAndEmpty)
{
    h = MinHeapCreateI64(4);
    ASSERT_EQ(MinHeapDelminI64(h, nullptr, nullptr), 0);  // empty
    ASSERT_EQ(MinHeapMinI64(h, nullptr, nullptr), 0);
    ASSERT_EQ(MinHeapInsertI64(h, 7, reinterpret_cast<void *>(9)), MINHEAP_OK);
    ASSERT_EQ(MinHeapMinI64(h, nullptr, nullptr), 1);
    ASSERT_EQ(MinHeapDelminI64(h, nullptr, nullptr), 1);
    EXPECT_EQ(MinHeapSize(h), 0);
}

TEST_F(MinHeapTest, I64ForeachRoundTrip)
{
    h = MinHeapCreateI64(8);
    const int64_t keys[] = {10, -5, 0, 42, 7};
    for (int64_t k : keys)
        ASSERT_EQ(MinHeapInsertI64(h, k, nullptr), MINHEAP_OK);
    AccI64 acc;
    EXPECT_EQ(MinHeapForeach(h, collect_i64, &acc), 5u);
    std::sort(acc.keys.begin(), acc.keys.end());
    const std::vector<int64_t> expect = {-5, 0, 7, 10, 42};
    EXPECT_EQ(acc.keys, expect);
}

TEST_F(MinHeapTest, I64ClearPreservesMode)
{
    h = MinHeapCreateI64(4);
    ASSERT_EQ(MinHeapInsertI64(h, 3, nullptr), MINHEAP_OK);
    MinHeapClear(h);
    EXPECT_EQ(MinHeapSize(h), 0);
    ASSERT_EQ(MinHeapInsertI64(h, 1, nullptr), MINHEAP_OK);  // still i64
    int64_t k = 0;
    ASSERT_EQ(MinHeapMinI64(h, &k, nullptr), 1);
    EXPECT_EQ(k, 1);
}

// ── Mode mismatch ──────────────────────────────────────────────────────

TEST_F(MinHeapTest, WrongModeRejected)
{
    h = MinHeapCreate(4, cmp_int);
    EXPECT_EQ(MinHeapInsertI64(h, 1, nullptr), MINHEAP_ERR_INVAL);
    EXPECT_EQ(MinHeapMinI64(h, nullptr, nullptr), 0);
    EXPECT_EQ(MinHeapDelminI64(h, nullptr, nullptr), 0);
    // min/delmin on a pointer heap still work.
    int k = 9;
    ASSERT_EQ(MinHeapInsert(h, &k, &k), MINHEAP_OK);
    void *out = nullptr;
    ASSERT_EQ(MinHeapMin(h, &out, nullptr), 1);
    EXPECT_EQ(*static_cast<int *>(out), 9);
}

// ── MinHeapDescribe introspection ──────────────────────────────────────

TEST_F(MinHeapTest, DescribeEmitsStateJson)
{
    h = MinHeapCreate(8, cmp_int);
    int a = 3, b = 7;
    ASSERT_EQ(MinHeapInsert(h, &a, &a), MINHEAP_OK);
    ASSERT_EQ(MinHeapInsert(h, &b, &b), MINHEAP_OK);

    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 512);
    ASSERT_NE(bd.data, nullptr);
    EXPECT_EQ(MinHeapDescribe(h, &bd), &bd);
    EXPECT_NE(strstr(bd.data, "\"size\":2"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"mode\":\"ptr\""), nullptr);
    EXPECT_NE(strstr(bd.data, "\"has_comparator\":true"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"entries\":["), nullptr);
    EXPECT_NE(strstr(bd.data, "\"index\":0"), nullptr);
    BufferDescriptorRelease(&bd);
}

TEST_F(MinHeapTest, DescribeAllocatesWhenProvidedNull)
{
    h = MinHeapCreate(4, cmp_int);
    int k = 9;
    ASSERT_EQ(MinHeapInsert(h, &k, &k), MINHEAP_OK);

    BufferDescriptor *out = MinHeapDescribe(h, nullptr);
    ASSERT_NE(out, nullptr);
    ASSERT_NE(out->data, nullptr);
    EXPECT_NE(strstr(out->data, "\"size\":1"), nullptr);
    BufferDescriptorRelease(out);
    free(out);
}

TEST_F(MinHeapTest, DescribeNullHandleEmitsError)
{
    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 128);
    ASSERT_NE(bd.data, nullptr);
    EXPECT_EQ(MinHeapDescribe(nullptr, &bd), &bd);
    EXPECT_NE(strstr(bd.data, "\"error\":\"null handle\""), nullptr);
    BufferDescriptorRelease(&bd);
}

TEST_F(MinHeapTest, DescribeI64Mode)
{
    h = MinHeapCreateI64(8);
    ASSERT_EQ(MinHeapInsertI64(h, 42, reinterpret_cast<void *>(0x1)), MINHEAP_OK);
    ASSERT_EQ(MinHeapInsertI64(h, -7, nullptr), MINHEAP_OK);

    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 512);
    ASSERT_NE(bd.data, nullptr);
    MinHeapDescribe(h, &bd);
    EXPECT_NE(strstr(bd.data, "\"mode\":\"i64\""), nullptr);
    EXPECT_NE(strstr(bd.data, "\"has_comparator\":false"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"key\":42"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"key\":-7"), nullptr);
    BufferDescriptorRelease(&bd);
}

}  // namespace
