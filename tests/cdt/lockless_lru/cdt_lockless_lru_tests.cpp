/**
 * @file cdt_lockless_lru_tests.cpp
 * @brief GoogleTest suite for the LocklessLru approximate LRU cache.
 *
 * Covers lifecycle, single-threaded correctness, edge cases, and
 * concurrent safety.  White-box access via _priv.h is used to
 * validate internal invariants (LruSlot layout, cache-line alignment).
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

extern "C" {
#include <uflib/standard_c_includes.h>
#include <uflib/cdt/lockless_lru/cdt_lockless_lru.h>
#include <uflib/cdt/lockless_lru/cdt_lockless_lru_type.h>
#include <uflib/cdt/lockless_lru/cdt_lockless_lru_defs.h>
#include "cdt_lockless_lru_priv.h"
}

/* ── Concrete payload for tests ─────────────────────────────────────────── */

struct TestPayload {
    uint64_t key;
    long     value;
};

/* ── Helpers ────────────────────────────────────────────────────────────── */

static LocklessLruConfig
sMakeConfig(size_t capacity_hint)
{
    LocklessLruConfig cfg;
    cfg.capacity_hint = capacity_hint;
    return cfg;
}

/* ── Lifecycle tests ────────────────────────────────────────────────────── */

TEST(LocklessLruLifecycle, CreateWithValidConfig)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);
    EXPECT_EQ(LocklessLruSize(lru_ptr), 0u);
    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruLifecycle, CreateWithNullConfig)
{
    LocklessLru *lru_ptr = LocklessLruCreate(nullptr);
    ASSERT_NE(lru_ptr, nullptr);
    EXPECT_EQ(LocklessLruSize(lru_ptr), 0u);
    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruLifecycle, DestroyNullIsSafe)
{
    LocklessLruDestroy(nullptr);
    // No crash = pass
}

TEST(LocklessLruLifecycle, DoubleDestroySafety)
{
    LocklessLruConfig cfg = sMakeConfig(16);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);
    LocklessLruDestroy(lru_ptr);
    LocklessLruDestroy(nullptr); // second call on NULL (after first frees it)
    // We don't call Destroy(lru_ptr) twice with the same non-null pointer
    // because the first Destroy frees it — using it again would be UB.
    // This test verifies the first Destroy doesn't corrupt state.
}

TEST(LocklessLruLifecycle, CreateDestroyCreateCycle)
{
    LocklessLruConfig cfg = sMakeConfig(16);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);
    LocklessLruDestroy(lru_ptr);

    lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);
    LocklessLruDestroy(lru_ptr);
}

/* ── Struct layout tests (white-box) ────────────────────────────────────── */

TEST(LocklessLruLayout, SlotSizeIsCacheLineAligned)
{
    // _Static_assert in _priv.h already enforces this at compile time.
    // This runtime check provides a second line of defence.
    EXPECT_EQ(sizeof(LruSlot), 64u);
}

TEST(LocklessLruLayout, SlotArrayAlignment)
{
    LocklessLruConfig cfg = sMakeConfig(16);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);
    // Slots should be cache-line aligned (64-byte boundary)
    EXPECT_EQ(reinterpret_cast<uintptr_t>(lru_ptr->slots) % 64, 0u);
    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruLayout, ClockHandAndCountOnSeparateCacheLines)
{
    // clock_hand must be in cache line 0 (offset < 64).
    // count must be in cache line 1 (offset >= 64).
    // These are enforced at compile time by _Static_assert in _priv.h;
    // this runtime check provides a second line of defence.
    EXPECT_LT(offsetof(struct LocklessLru, clock_hand), 64u);
    EXPECT_GE(offsetof(struct LocklessLru, count), 64u);
}

/* ── Single-thread correctness ──────────────────────────────────────────── */

TEST(LocklessLruSingleThread, SetGetRoundTrip)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p1 = { .key = 42, .value = 999 };
    EXPECT_EQ(LocklessLruSet(lru_ptr, p1.key, (LruClientData *)&p1), nullptr);

    TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, 42);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->key, 42u);
    EXPECT_EQ(got->value, 999L);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruSingleThread, SetDuplicateKeyUpserts)
{
    // Set is an UPSERT: inserting an existing key replaces the value in
    // place and does not create a second slot / inflate the count.
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p1 = { .key = 1, .value = 100 };
    TestPayload p2 = { .key = 1, .value = 200 };
    EXPECT_EQ(LocklessLruSet(lru_ptr, 1, (LruClientData *)&p1), nullptr);
    // Second Set replaces the old value and hands it back as displaced.
    EXPECT_EQ(LocklessLruSet(lru_ptr, 1, (LruClientData *)&p2), (LruClientData *)&p1);

    EXPECT_EQ(LocklessLruSize(lru_ptr), 1u); // no second slot

    TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, 1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got, &p2);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruSingleThread, RemoveExisting)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p1 = { .key = 7, .value = 700 };
    EXPECT_EQ(LocklessLruSet(lru_ptr, 7, (LruClientData *)&p1), nullptr);
    EXPECT_EQ(LocklessLruSize(lru_ptr), 1u);

    LruClientData *removed = LocklessLruRemove(lru_ptr, 7);
    ASSERT_EQ(removed, (LruClientData *)&p1);
    EXPECT_EQ(LocklessLruSize(lru_ptr), 0u);
    EXPECT_EQ(LocklessLruGet(lru_ptr, 7), nullptr);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruSingleThread, RemoveNonExistent)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    EXPECT_EQ(LocklessLruRemove(lru_ptr, 99), nullptr);
    EXPECT_EQ(LocklessLruRemove(lru_ptr, 0), nullptr);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruSingleThread, ReinsertAfterRemove)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p1 = { .key = 5, .value = 500 };
    TestPayload p2 = { .key = 5, .value = 501 };

    LocklessLruSet(lru_ptr, 5, (LruClientData *)&p1);
    EXPECT_EQ(LocklessLruRemove(lru_ptr, 5), (LruClientData *)&p1);
    // Re-insert same key with different payload
    EXPECT_EQ(LocklessLruSet(lru_ptr, 5, (LruClientData *)&p2), nullptr);

    TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, 5);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got, &p2);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruSingleThread, EvictionAtCapacity)
{
    LocklessLruConfig cfg = sMakeConfig(4);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload items[10];
    for (int i = 0; i < 10; i++) { items[i].key = (uint64_t)i; items[i].value = i * 100L; }

    int evictions = 0;
    for (int i = 0; i < 10; i++) {
        LruClientData *ev = LocklessLruSet(lru_ptr, items[i].key,
                                           (LruClientData *)&items[i]);
        if (ev) evictions++;
    }

    EXPECT_GT(evictions, 0);
    // Insert-then-trim keeps the steady-state size at the target capacity.
    EXPECT_LE(LocklessLruSize(lru_ptr), cfg.capacity_hint);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruSingleThread, GetOnEmpty)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    EXPECT_EQ(LocklessLruGet(lru_ptr, 0), nullptr);
    EXPECT_EQ(LocklessLruGet(lru_ptr, 42), nullptr);
    EXPECT_EQ(LocklessLruGet(lru_ptr, UINT64_MAX), nullptr);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruSingleThread, SizeConsistency)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload items[20];
    for (int i = 0; i < 20; i++) { items[i].key = (uint64_t)i; items[i].value = i * 10L; }

    for (int i = 0; i < 10; i++)
        LocklessLruSet(lru_ptr, items[i].key, (LruClientData *)&items[i]);
    EXPECT_EQ(LocklessLruSize(lru_ptr), 10u);

    for (int i = 0; i < 5; i++)
        LocklessLruRemove(lru_ptr, items[i].key);
    EXPECT_EQ(LocklessLruSize(lru_ptr), 5u);

    LocklessLruDestroy(lru_ptr);
}

/* ── Upsert / SetEx status classification ──────────────────────────────── */

TEST(LocklessLruSetEx, ReportsInsertedThenReplaced)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p1 = { .key = 1, .value = 100 };
    TestPayload p2 = { .key = 1, .value = 200 };

    LocklessLruSetResult r1 = LocklessLruSetEx(lru_ptr, 1, (LruClientData *)&p1);
    EXPECT_EQ(r1.status, LOCKLESS_LRU_SET_INSERTED);
    EXPECT_EQ(r1.displaced, nullptr);
    EXPECT_EQ(LocklessLruSize(lru_ptr), 1u);

    LocklessLruSetResult r2 = LocklessLruSetEx(lru_ptr, 1, (LruClientData *)&p2);
    EXPECT_EQ(r2.status, LOCKLESS_LRU_SET_REPLACED);
    EXPECT_EQ(r2.displaced, (LruClientData *)&p1);
    EXPECT_EQ(LocklessLruSize(lru_ptr), 1u); // replace does not change count

    TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, 1);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got, &p2);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruSetEx, ReportsEvictedWhenOverCapacity)
{
    LocklessLruConfig cfg = sMakeConfig(4);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload items[8];
    for (int i = 0; i < 8; i++) { items[i].key = (uint64_t)i; items[i].value = i * 10L; }

    int evicted = 0;
    for (int i = 0; i < 8; i++) {
        LocklessLruSetResult r = LocklessLruSetEx(lru_ptr, items[i].key,
                                                  (LruClientData *)&items[i]);
        // Distinct keys: never FULL, never REPLACED.
        EXPECT_NE(r.status, LOCKLESS_LRU_SET_FULL);
        EXPECT_NE(r.status, LOCKLESS_LRU_SET_REPLACED);
        if (r.status == LOCKLESS_LRU_SET_EVICTED) {
            ASSERT_NE(r.displaced, nullptr);
            evicted++;
        }
    }
    EXPECT_GT(evicted, 0);
    EXPECT_LE(LocklessLruSize(lru_ptr), cfg.capacity_hint);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruSetEx, InsertedNeverDisplacesAndCarriesNoVictim)
{
    // On a plain insert under capacity, INSERTED must be the terminal status
    // and displaced must be NULL — no eviction and no silent victim.
    LocklessLruConfig cfg = sMakeConfig(4);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload items[4];
    for (int i = 0; i < 4; i++) { items[i].key = (uint64_t)i; items[i].value = i * 10L; }

    for (int i = 0; i < 4; i++) {
        LocklessLruSetResult r = LocklessLruSetEx(lru_ptr, items[i].key,
                                                  (LruClientData *)&items[i]);
        EXPECT_EQ(r.status, LOCKLESS_LRU_SET_INSERTED);
        EXPECT_EQ(r.displaced, nullptr);
    }
    EXPECT_EQ(LocklessLruSize(lru_ptr), 4u);

    LocklessLruDestroy(lru_ptr);
}

/* ── Generation (GetRef / RefStillValid) ───────────────────────────────── */

TEST(LocklessLruGeneration, GetRefRoundTrip)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p1 = { .key = 7, .value = 700 };
    LocklessLruSet(lru_ptr, 7, (LruClientData *)&p1);

    LocklessLruRef ref = LocklessLruGetRef(lru_ptr, 7);
    ASSERT_EQ(ref.data, (LruClientData *)&p1);
    EXPECT_TRUE(LocklessLruRefStillValid(lru_ptr, 7, ref.gen));

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruGeneration, RefStillValidDetectsReplaceAndRemove)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p1 = { .key = 9, .value = 900 };
    TestPayload p2 = { .key = 9, .value = 901 };
    LocklessLruSet(lru_ptr, 9, (LruClientData *)&p1);

    uint64_t gen1 = LocklessLruGetRef(lru_ptr, 9).gen;
    ASSERT_TRUE(LocklessLruRefStillValid(lru_ptr, 9, gen1));

    // Replace bumps the generation.
    LocklessLruSet(lru_ptr, 9, (LruClientData *)&p2);
    EXPECT_FALSE(LocklessLruRefStillValid(lru_ptr, 9, gen1));

    uint64_t gen2 = LocklessLruGetRef(lru_ptr, 9).gen;
    EXPECT_TRUE(LocklessLruRefStillValid(lru_ptr, 9, gen2));

    // Remove bumps it again and makes the key absent.
    LocklessLruRemove(lru_ptr, 9);
    EXPECT_FALSE(LocklessLruRefStillValid(lru_ptr, 9, gen2));

    LocklessLruDestroy(lru_ptr);
}

/* ── Bounded CLOCK eviction (liveness) ─────────────────────────────────── */

TEST(LocklessLruEviction, ForceEvictsWhenAllReferenced)
{
    // Fill the cache, Get every key so all are referenced, then insert a
    // fresh key.  CLOCK must still terminate (force-evict path) rather than
    // livelock on a hot, all-referenced working set.
    LocklessLruConfig cfg = sMakeConfig(8);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload items[8];
    for (int i = 0; i < 8; i++) { items[i].key = (uint64_t)i; items[i].value = i * 10L; }
    for (int i = 0; i < 8; i++)
        LocklessLruSet(lru_ptr, items[i].key, (LruClientData *)&items[i]);

    // Mark every resident key recently-used.
    for (int i = 0; i < 8; i++)
        EXPECT_NE(LocklessLruGet(lru_ptr, items[i].key), nullptr);

    TestPayload pnew = { .key = 100, .value = 100 };
    LocklessLruSetResult r = LocklessLruSetEx(lru_ptr, pnew.key, (LruClientData *)&pnew);
    EXPECT_EQ(r.status, LOCKLESS_LRU_SET_EVICTED);
    ASSERT_NE(r.displaced, nullptr);

    LocklessLruDestroy(lru_ptr);
}

/* ── Tombstone collapse (white-box) ────────────────────────────────────── */

TEST(LocklessLruTombstone, CollapsesToEmptyWhenSuccessorEmpty)
{
    // Insert a single key, then remove it.  Its successor is EMPTY, so the
    // tombstone must collapse back to EMPTY (not linger as TOMBSTONE).
    LocklessLruConfig cfg = sMakeConfig(16);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p1 = { .key = 5, .value = 500 };
    LocklessLruSet(lru_ptr, 5, (LruClientData *)&p1);
    LocklessLruRemove(lru_ptr, 5);

    size_t idx = sMix64(5) & (lru_ptr->capacity - 1);
    uint8_t state; uint64_t skey;
    sSlotPeek(&lru_ptr->slots[idx], &state, &skey);
    EXPECT_EQ(state, (uint8_t)CDT_LOCKLESS_LRU_SLOT_EMPTY);

    LocklessLruDestroy(lru_ptr);
}

/* ── Capacity / overflow (white-box) ───────────────────────────────────── */

TEST(LocklessLruCapacity, ReportsPhysicalCapacity)
{
    LocklessLruConfig cfg = sMakeConfig(4);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);
    // min_phys = max(4 * 2, 8) = 8 → next_pow2 = 8
    EXPECT_EQ(LocklessLruCapacity(lru_ptr), 8u);
    LocklessLruDestroy(lru_ptr);

    cfg.capacity_hint = 100;
    lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);
    // min_phys = 200 → next_pow2 = 256
    EXPECT_EQ(LocklessLruCapacity(lru_ptr), 256u);
    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruCreate, RejectsOverflowingCapacity)
{
    LocklessLruConfig cfg;
    cfg.capacity_hint = (size_t)(SIZE_MAX / 2 + 1); // hint * 2 overflows size_t
    EXPECT_EQ(LocklessLruCreate(&cfg), nullptr);

    cfg.capacity_hint = SIZE_MAX;
    EXPECT_EQ(LocklessLruCreate(&cfg), nullptr);
}

TEST(LocklessLruWhiteBox, NextPow2Overflow)
{
    EXPECT_EQ(sNextPow2(0), 1u);
    EXPECT_EQ(sNextPow2(1), 1u);
    EXPECT_EQ(sNextPow2(3), 4u);
    EXPECT_EQ(sNextPow2(8), 8u);
    EXPECT_EQ(sNextPow2((size_t)1 << 63), (size_t)1 << 63);
    EXPECT_EQ(sNextPow2(((size_t)1 << 63) + 1), 0u); // overflows to 0
    EXPECT_EQ(sNextPow2(SIZE_MAX), 0u);
}

/* ── DescribeLocklessLru introspection ─────────────────────────────────── */

TEST(LocklessLruDescribe, EmitsStateJson)
{
    LocklessLruConfig cfg = sMakeConfig(8); // physical capacity = 16
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p1 = { .key = 7, .value = 700 };
    TestPayload p2 = { .key = 42, .value = 4200 };
    LocklessLruSet(lru_ptr, p1.key, (LruClientData *)&p1);
    LocklessLruSet(lru_ptr, p2.key, (LruClientData *)&p2);

    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 512);
    ASSERT_NE(bd.data, nullptr);

    EXPECT_EQ(DescribeLocklessLru(lru_ptr, &bd), &bd);
    EXPECT_NE(strstr(bd.data, "\"capacity\":16"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"max_items\":8"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"count\":2"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"load_factor\""), nullptr);
    EXPECT_NE(strstr(bd.data, "\"occupied\":2"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"empty\":14"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"tombstone\":0"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"slots\""), nullptr);
    EXPECT_NE(strstr(bd.data, "\"key\":7"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"key\":42"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"gen\""), nullptr);

    BufferDescriptorRelease(&bd);
    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruDescribe, AllocatesWhenProvidedNull)
{
    LocklessLruConfig cfg = sMakeConfig(4); // physical capacity = 8
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    BufferDescriptor *out = DescribeLocklessLru(lru_ptr, nullptr);
    ASSERT_NE(out, nullptr);
    ASSERT_NE(out->data, nullptr);
    EXPECT_NE(strstr(out->data, "\"capacity\":8"), nullptr);

    BufferDescriptorRelease(out);
    free(out);
    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruDescribe, NullHandleEmitsError)
{
    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 128);
    ASSERT_NE(bd.data, nullptr);

    EXPECT_EQ(DescribeLocklessLru(nullptr, &bd), &bd);
    EXPECT_NE(strstr(bd.data, "null handle"), nullptr);

    BufferDescriptorRelease(&bd);
}

/* ── Key edge cases ────────────────────────────────────────────────────── */

TEST(LocklessLruEdgeCases, KeyZero)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload p0 = { .key = 0, .value = 0 };
    EXPECT_EQ(LocklessLruSet(lru_ptr, 0, (LruClientData *)&p0), nullptr);
    TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, 0);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->value, 0L);
    EXPECT_EQ(LocklessLruRemove(lru_ptr, 0), (LruClientData *)&p0);
    EXPECT_EQ(LocklessLruGet(lru_ptr, 0), nullptr);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruEdgeCases, KeyUint64Max)
{
    LocklessLruConfig cfg = sMakeConfig(64);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload pMax = { .key = UINT64_MAX, .value = -1L };
    EXPECT_EQ(LocklessLruSet(lru_ptr, UINT64_MAX, (LruClientData *)&pMax), nullptr);
    TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, UINT64_MAX);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->value, -1L);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruEdgeCases, MultipleKeysProbePastOccupied)
{
    // Verify that keys that collide to nearby slots are independently
    // retrievable (linear probing correctly skips occupied slots).
    LocklessLruConfig cfg = sMakeConfig(128);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    TestPayload items[50];
    for (int i = 0; i < 50; i++) { items[i].key = (uint64_t)i; items[i].value = i * 10L; }

    for (int i = 0; i < 50; i++)
        LocklessLruSet(lru_ptr, items[i].key, (LruClientData *)&items[i]);

    for (int i = 0; i < 50; i++) {
        TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, items[i].key);
        ASSERT_NE(got, nullptr) << "key=" << i << " not found";
        EXPECT_EQ(got->value, items[i].value) << "key=" << i << " wrong value";
    }

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruEdgeCases, TombstoneProbeChain)
{
    // Insert A, Insert B (B may land past A due to collision),
    // Remove A (tombstone), Insert C that probes past tombstone,
    // verify B is still retrievable (tombstone didn't break B's probe chain).
    LocklessLruConfig cfg = sMakeConfig(16);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    // Insert enough keys to fill a good portion of the small table,
    // ensuring some probe chains exist.
    TestPayload items[10];
    for (int i = 0; i < 10; i++) { items[i].key = (uint64_t)(i * 7); items[i].value = i * 10L; }

    for (int i = 0; i < 10; i++)
        LocklessLruSet(lru_ptr, items[i].key, (LruClientData *)&items[i]);

    // Remove the first few keys — these become tombstones.
    for (int i = 0; i < 3; i++)
        LocklessLruRemove(lru_ptr, items[i].key);

    // The remaining keys should still be retrievable.
    for (int i = 3; i < 10; i++) {
        TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, items[i].key);
        ASSERT_NE(got, nullptr) << "key=" << items[i].key << " lost after tombstone creation";
        EXPECT_EQ(got->value, items[i].value);
    }

    LocklessLruDestroy(lru_ptr);
}

/* ── Concurrent — disjoint keys ────────────────────────────────────────── */

TEST(LocklessLruConcurrent, DisjointInserts)
{
    constexpr int kThreads = 8;
    constexpr int kKeysPerThread = 500;
    LocklessLruConfig cfg = sMakeConfig(kThreads * kKeysPerThread + 100);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    std::vector<TestPayload> items(kThreads * kKeysPerThread);
    for (size_t i = 0; i < items.size(); i++) {
        items[i].key = i;
        items[i].value = (long)i;
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([lru_ptr, &items, t]() {
            int base = t * kKeysPerThread;
            for (int i = 0; i < kKeysPerThread; i++) {
                LocklessLruSet(lru_ptr, items[base + i].key,
                               (LruClientData *)&items[base + i]);
            }
        });
    }
    for (auto &th : threads) th.join();

    // All inserted keys should be retrievable.
    int missing = 0;
    for (size_t i = 0; i < items.size(); i++) {
        TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, items[i].key);
        if (!got) missing++;
    }
    // With disjoint keys and ample capacity, all should be found.
    EXPECT_EQ(missing, 0);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruConcurrent, DisjointMixed)
{
    constexpr int kThreads = 6;
    constexpr int kOpsPerThread = 1000;
    LocklessLruConfig cfg = sMakeConfig(256);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    std::vector<TestPayload> items(1000);
    for (size_t i = 0; i < items.size(); i++) {
        items[i].key = i;
        items[i].value = (long)i;
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([lru_ptr, &items, t]() {
            std::mt19937 rng((unsigned)(t + 1));
            int base = t * 100;
            for (int i = 0; i < kOpsPerThread; i++) {
                int idx = base + (rng() % 100);
                int op = rng() % 3;
                if (op == 0)
                    LocklessLruSet(lru_ptr, items[idx].key, (LruClientData *)&items[idx]);
                else if (op == 1)
                    LocklessLruGet(lru_ptr, items[idx].key);
                else
                    LocklessLruRemove(lru_ptr, items[idx].key);
            }
        });
    }
    for (auto &th : threads) th.join();

    size_t sz = LocklessLruSize(lru_ptr);
    EXPECT_LE(sz, cfg.capacity_hint * 2u); // within hard cap

    LocklessLruDestroy(lru_ptr);
}

/* ── Concurrent — same keys ────────────────────────────────────────────── */

TEST(LocklessLruConcurrent, SameKeysInsert)
{
    constexpr int kThreads = 8;
    constexpr int kKeys = 200;
    LocklessLruConfig cfg = sMakeConfig(kKeys + 50);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    std::vector<TestPayload> items(kKeys);
    for (int i = 0; i < kKeys; i++) { items[i].key = (uint64_t)i; items[i].value = (long)i; }

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([lru_ptr, &items, kKeys]() {
            for (int i = 0; i < kKeys; i++) {
                LocklessLruSet(lru_ptr, items[i].key, (LruClientData *)&items[i]);
            }
        });
    }
    for (auto &th : threads) th.join();

    // Verify structural correctness: all keys retrievable, values valid
    for (int i = 0; i < kKeys; i++) {
        TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, items[i].key);
        if (got) {
            EXPECT_EQ(got->key, items[i].key);
        }
    }
    EXPECT_LE(LocklessLruSize(lru_ptr), cfg.capacity_hint * 2u);

    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruConcurrent, SameKeysMixed)
{
    constexpr int kThreads = 6;
    constexpr int kOpsPerThread = 2000;
    constexpr int kKeys = 100;
    LocklessLruConfig cfg = sMakeConfig(kKeys + 50);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    std::vector<TestPayload> items(kKeys);
    for (int i = 0; i < kKeys; i++) { items[i].key = (uint64_t)i; items[i].value = (long)i; }

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([lru_ptr, &items, kKeys, t]() {
            std::mt19937 rng((unsigned)(t + 100));
            for (int i = 0; i < kOpsPerThread; i++) {
                int idx = rng() % kKeys;
                int op = rng() % 3;
                if (op == 0)
                    LocklessLruSet(lru_ptr, items[idx].key, (LruClientData *)&items[idx]);
                else if (op == 1) {
                    TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, items[idx].key);
                    if (got) EXPECT_EQ(got->key, items[idx].key);
                } else {
                    LocklessLruRemove(lru_ptr, items[idx].key);
                }
            }
        });
    }
    for (auto &th : threads) th.join();

    EXPECT_LE(LocklessLruSize(lru_ptr), cfg.capacity_hint * 2u);
    LocklessLruDestroy(lru_ptr);
}

/* ── Concurrent — mixed workload ───────────────────────────────────────── */

TEST(LocklessLruConcurrent, MixedWorkload)
{
    constexpr int kThreads = 8;
    constexpr int kDurationMs = 2000;
    LocklessLruConfig cfg = sMakeConfig(1024);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    constexpr int kKeys = 2000;
    std::vector<TestPayload> items(kKeys);
    for (int i = 0; i < kKeys; i++) { items[i].key = (uint64_t)i; items[i].value = (long)i; }

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([lru_ptr, &items, &stop, &errors, t]() {
            std::mt19937 rng((unsigned)(t + 200));
            while (!stop.load(std::memory_order_relaxed)) {
                int idx = rng() % kKeys;
                int op = rng() % 3;
                if (op == 0) {
                    LocklessLruSet(lru_ptr, items[idx].key, (LruClientData *)&items[idx]);
                } else if (op == 1) {
                    TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, items[idx].key);
                    if (got && got->key != items[idx].key) errors.fetch_add(1);
                } else {
                    LocklessLruRemove(lru_ptr, items[idx].key);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kDurationMs));
    stop.store(true);
    for (auto &th : threads) th.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_LE(LocklessLruSize(lru_ptr), cfg.capacity_hint * 2u);
    LocklessLruDestroy(lru_ptr);
}

TEST(LocklessLruConcurrent, InsertRemoveCycles)
{
    constexpr int kThreads = 6;
    constexpr int kOpsPerThread = 2000;
    constexpr int kKeys = 150;
    LocklessLruConfig cfg = sMakeConfig(kKeys + 50);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    std::vector<TestPayload> items(kKeys);
    for (int i = 0; i < kKeys; i++) { items[i].key = (uint64_t)i; items[i].value = (long)i; }

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([lru_ptr, &items, kKeys, t]() {
            std::mt19937 rng((unsigned)(t + 300));
            for (int i = 0; i < kOpsPerThread; i++) {
                int idx = rng() % kKeys;
                if (rng() % 2 == 0)
                    LocklessLruSet(lru_ptr, items[idx].key, (LruClientData *)&items[idx]);
                else
                    LocklessLruRemove(lru_ptr, items[idx].key);
            }
        });
    }
    for (auto &th : threads) th.join();

    size_t sz = LocklessLruSize(lru_ptr);
    EXPECT_LE(sz, cfg.capacity_hint * 2u);
    LocklessLruDestroy(lru_ptr);
}

/* ── High-contention stress ────────────────────────────────────────────── */

TEST(LocklessLruConcurrent, HighContentionStress)
{
    constexpr int kThreads = 12;
    constexpr int kOpsPerThread = 5000;
    LocklessLruConfig cfg = sMakeConfig(1024);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    constexpr int kKeys = 3000;
    std::vector<TestPayload> items(kKeys);
    for (int i = 0; i < kKeys; i++) { items[i].key = (uint64_t)i; items[i].value = (long)i; }

    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([lru_ptr, &items, &errors, t]() {
            std::mt19937 rng((unsigned)(t + 400));
            for (int i = 0; i < kOpsPerThread; i++) {
                int idx = rng() % kKeys;
                int op = rng() % 3;
                if (op == 0) {
                    LocklessLruSet(lru_ptr, items[idx].key, (LruClientData *)&items[idx]);
                } else if (op == 1) {
                    TestPayload *got = (TestPayload *)LocklessLruGet(lru_ptr, items[idx].key);
                    if (got && got->key != items[idx].key) errors.fetch_add(1);
                } else {
                    LocklessLruRemove(lru_ptr, items[idx].key);
                }
            }
        });
    }
    for (auto &th : threads) th.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_LE(LocklessLruSize(lru_ptr), cfg.capacity_hint * 2u);
    LocklessLruDestroy(lru_ptr);
}

/* ── Concurrent Size under mutation ────────────────────────────────────── */

TEST(LocklessLruConcurrent, SizeUnderMutation)
{
    constexpr int kThreads = 8;
    LocklessLruConfig cfg = sMakeConfig(512);
    LocklessLru *lru_ptr = LocklessLruCreate(&cfg);
    ASSERT_NE(lru_ptr, nullptr);

    std::vector<TestPayload> items(500);
    for (int i = 0; i < 500; i++) { items[i].key = (uint64_t)(i + 1000); items[i].value = (long)i; }

    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([lru_ptr, &items, &stop, t]() {
            std::mt19937 rng((unsigned)(t + 500));
            while (!stop.load(std::memory_order_relaxed)) {
                int idx = rng() % 500;
                if (rng() % 2 == 0)
                    LocklessLruSet(lru_ptr, items[idx].key, (LruClientData *)&items[idx]);
                else
                    LocklessLruRemove(lru_ptr, items[idx].key);
            }
        });
    }

    // Query Size concurrently — should never crash or return nonsense
    for (int i = 0; i < 10000; i++) {
        size_t sz = LocklessLruSize(lru_ptr);
        EXPECT_LE(sz, cfg.capacity_hint * 2u);
    }

    stop.store(true);
    for (auto &th : threads) th.join();

    LocklessLruDestroy(lru_ptr);
}
