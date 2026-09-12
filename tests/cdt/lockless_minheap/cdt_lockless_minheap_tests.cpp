/**
 * @file cdt_lockless_minheap_tests.cpp
 * @brief GoogleTest suite for the LocklessMinHeap lock-free min-PQ.
 *
 * Adversarial by intent: alongside the happy path it drives NULL handles,
 * INT64 boundary keys, equal-key contention (the mark/unlink helping path),
 * MPMC unique-ticket extraction, and a single-threaded random-op model
 * check against std::multiset.  Reclaim/destroy paths are exercised so that
 * ASan/LSan can catch double-free and leak regressions.
 *
 * White-box access via cdt_lockless_minheap_priv.h is limited to validating
 * the internal layout invariants the reclaim path depends on.
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
#include <climits>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

extern "C" {
#include <uflib/standard_c_includes.h>
#include <uflib/cdt/lockless_minheap/cdt_lockless_minheap.h>
#include <uflib/cdt/lockless_minheap/cdt_lockless_minheap_type.h>
#include "cdt_lockless_minheap_priv.h"
}

namespace {

/* ── Lifecycle & handle hygiene ──────────────────────────────────────── */

TEST(LocklessMinHeapLifecycle, CreateDestroyEmpty)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(LocklessMinHeapSizeApprox(h), 0u);

    int64_t k = 0;
    void *v = nullptr;
    EXPECT_EQ(LocklessMinHeapDelminI64(h, &k, &v), 0);  /* empty */

    LocklessMinHeapDestroy(h);
    LocklessMinHeapDestroy(nullptr);  /* no-op */
}

TEST(LocklessMinHeapLifecycle, NullHandlesDegradeGracefully)
{
    EXPECT_EQ(LocklessMinHeapInsertI64(nullptr, 1, nullptr), LOCKLESS_MINHEAP_ERR_INVAL);
    EXPECT_EQ(LocklessMinHeapDelminI64(nullptr, nullptr, nullptr), 0);
    EXPECT_EQ(LocklessMinHeapSizeApprox(nullptr), 0u);
    LocklessMinHeapReclaim(nullptr);   /* no-op */
}

TEST(LocklessMinHeapLifecycle, DestroyWithLiveNodesFreesAll)
{
    /* No delmin — destroy must free every live node plus the sentinel and
     * the retire stack.  Run under LSan to catch leaks. */
    LocklessMinHeap *h = LocklessMinHeapCreate();
    for (int i = 0; i < 1000; ++i)
        EXPECT_EQ(LocklessMinHeapInsertI64(h, i, nullptr), LOCKLESS_MINHEAP_OK);
    LocklessMinHeapDestroy(h);
}

/* ── Single-thread correctness ────────────────────────────────────────── */

TEST(LocklessMinHeapSingle, SingleInsertDelmin)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    ASSERT_EQ(LocklessMinHeapInsertI64(h, 42, (void *)0x7), LOCKLESS_MINHEAP_OK);
    EXPECT_EQ(LocklessMinHeapSizeApprox(h), 1u);

    int64_t k = 0;
    void *v = nullptr;
    ASSERT_EQ(LocklessMinHeapDelminI64(h, &k, &v), 1);
    EXPECT_EQ(k, 42);
    EXPECT_EQ(v, (void *)0x7);
    EXPECT_EQ(LocklessMinHeapDelminI64(h, &k, &v), 0);
    LocklessMinHeapDestroy(h);
}

TEST(LocklessMinHeapSingle, ExtractSorted)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    int keys[] = {9, 1, 5, 3, 7, 2, 8, 0, 4, 6};
    for (int k : keys)
        ASSERT_EQ(LocklessMinHeapInsertI64(h, k, nullptr), LOCKLESS_MINHEAP_OK);

    int64_t prev = INT64_MIN;
    for (int i = 0; i < 10; ++i) {
        int64_t k;
        ASSERT_EQ(LocklessMinHeapDelminI64(h, &k, nullptr), 1);
        EXPECT_GE(k, prev);
        prev = k;
    }
    EXPECT_EQ(LocklessMinHeapDelminI64(h, nullptr, nullptr), 0);
    LocklessMinHeapDestroy(h);
}

TEST(LocklessMinHeapSingle, BoundaryKeys)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    int64_t keys[] = {INT64_MIN, INT64_MAX, 0, -1, 1, INT64_MIN + 1, INT64_MAX - 1};
    for (int64_t k : keys)
        ASSERT_EQ(LocklessMinHeapInsertI64(h, k, nullptr), LOCKLESS_MINHEAP_OK);

    int64_t prev = INT64_MIN;
    for (int i = 0; i < 7; ++i) {
        int64_t k;
        ASSERT_EQ(LocklessMinHeapDelminI64(h, &k, nullptr), 1);
        EXPECT_GE(k, prev);
        prev = k;
    }
    EXPECT_EQ(LocklessMinHeapDelminI64(h, nullptr, nullptr), 0);
    LocklessMinHeapDestroy(h);
}

TEST(LocklessMinHeapSingle, DuplicatesAreAMultiset)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    for (int i = 0; i < 5; ++i)
        ASSERT_EQ(LocklessMinHeapInsertI64(h, 3, nullptr), LOCKLESS_MINHEAP_OK);
    for (int i = 0; i < 5; ++i) {
        int64_t k;
        ASSERT_EQ(LocklessMinHeapDelminI64(h, &k, nullptr), 1);
        EXPECT_EQ(k, 3);
    }
    EXPECT_EQ(LocklessMinHeapDelminI64(h, nullptr, nullptr), 0);
    LocklessMinHeapDestroy(h);
}

TEST(LocklessMinHeapSingle, OptionalOutParams)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    ASSERT_EQ(LocklessMinHeapInsertI64(h, 1, (void *)0x1), LOCKLESS_MINHEAP_OK);
    ASSERT_EQ(LocklessMinHeapDelminI64(h, nullptr, nullptr), 1);
    LocklessMinHeapDestroy(h);
}

TEST(LocklessMinHeapSingle, RandomOpsMatchModelMultiset)
{
    /* Single-threaded adversarial oracle: every delmin must return the model's
     * current minimum. */
    LocklessMinHeap *h = LocklessMinHeapCreate();
    std::multiset<int64_t> model;
    std::mt19937 rng(0xC0FFEEu);

    for (int i = 0; i < 20000; ++i) {
        if (rng() % 3 == 0 && !model.empty()) {
            int64_t k;
            void *v;
            ASSERT_EQ(LocklessMinHeapDelminI64(h, &k, &v), 1);
            int64_t m = *model.begin();
            model.erase(model.begin());
            EXPECT_EQ(k, m) << "iteration " << i;
        } else {
            int64_t key = (int64_t)(rng() % 2000) - 1000;
            ASSERT_EQ(LocklessMinHeapInsertI64(h, key, (void *)(intptr_t)key),
                      LOCKLESS_MINHEAP_OK);
            model.insert(key);
        }
    }
    while (!model.empty()) {
        int64_t k;
        ASSERT_EQ(LocklessMinHeapDelminI64(h, &k, nullptr), 1);
        int64_t m = *model.begin();
        model.erase(model.begin());
        EXPECT_EQ(k, m);
    }
    EXPECT_EQ(LocklessMinHeapDelminI64(h, nullptr, nullptr), 0);
    EXPECT_EQ(LocklessMinHeapSizeApprox(h), 0u);
    LocklessMinHeapDestroy(h);
}

/* ── Reclamation ───────────────────────────────────────────────────────── */

TEST(LocklessMinHeapReclaim, ReclaimIdempotentAfterPartialDrain)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    for (int i = 0; i < 100; ++i)
        ASSERT_EQ(LocklessMinHeapInsertI64(h, i, nullptr), LOCKLESS_MINHEAP_OK);
    for (int i = 0; i < 50; ++i) {   /* retire 50 nodes */
        int64_t k;
        ASSERT_EQ(LocklessMinHeapDelminI64(h, &k, nullptr), 1);
    }
    LocklessMinHeapReclaim(h);   /* free 50 retired */
    LocklessMinHeapReclaim(h);   /* idempotent — no double free (ASan) */
    LocklessMinHeapDestroy(h);   /* free 50 live + sentinel */
}

/* ── Concurrency ───────────────────────────────────────────────────────── */

TEST(LocklessMinHeapConcurrent, ConcurrentInsertThenSerialDrainSorted)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    const int T = 4, N = 500;
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t) {
        th.emplace_back([=] {
            for (int i = 0; i < N; ++i)
                EXPECT_EQ(LocklessMinHeapInsertI64(h, (int64_t)t * N + i, nullptr),
                          LOCKLESS_MINHEAP_OK);
        });
    }
    for (auto &x : th)
        x.join();

    EXPECT_EQ(LocklessMinHeapSizeApprox(h), (size_t)T * N);

    int64_t prev = INT64_MIN, got = 0, k;
    while (LocklessMinHeapDelminI64(h, &k, nullptr)) {
        EXPECT_GE(k, prev);
        prev = k;
        ++got;
    }
    EXPECT_EQ(got, T * N);
    LocklessMinHeapReclaim(h);
    LocklessMinHeapDestroy(h);
}

TEST(LocklessMinHeapConcurrent, MpmcUniqueTicketsNoLostNoDouble)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    const int P = 4, C = 4, N = 1000;
    std::atomic<int> produced{0}, consumed{0}, oob{0};
    std::vector<int> seen(P * N, 0);
    std::mutex mu;
    std::vector<std::thread> th;

    for (int t = 0; t < P; ++t) {
        th.emplace_back([&, t] {
            for (int i = 0; i < N; ++i) {
                int64_t key = (int64_t)t * N + i;
                EXPECT_EQ(LocklessMinHeapInsertI64(h, key, (void *)(intptr_t)(key + 1)),
                          LOCKLESS_MINHEAP_OK);
                produced.fetch_add(1);
            }
        });
    }
    for (int c = 0; c < C; ++c) {
        th.emplace_back([&] {
            int64_t k;
            void *v;
            for (;;) {
                if (LocklessMinHeapDelminI64(h, &k, &v)) {
                    if (k < 0 || k >= P * N) {
                        oob.fetch_add(1);
                        continue;
                    }
                    if (v != (void *)(intptr_t)(k + 1)) {
                        oob.fetch_add(1);
                    }
                    {
                        std::lock_guard<std::mutex> g(mu);
                        seen[(int)k]++;
                    }
                    consumed.fetch_add(1);
                } else if (produced.load() == P * N && consumed.load() == P * N) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto &x : th)
        x.join();

    /* Final serial drain to catch any straggler in the extraction window. */
    int64_t k;
    while (LocklessMinHeapDelminI64(h, &k, nullptr)) {
        if (k < 0 || k >= P * N) {
            oob.fetch_add(1);
            continue;
        }
        {
            std::lock_guard<std::mutex> g(mu);
            seen[(int)k]++;
        }
        consumed.fetch_add(1);
    }

    EXPECT_EQ(oob.load(), 0);
    EXPECT_EQ(consumed.load(), P * N);
    EXPECT_EQ(LocklessMinHeapSizeApprox(h), 0u);
    for (int i = 0; i < P * N; ++i)
        EXPECT_EQ(seen[i], 1) << "ticket " << i;
    LocklessMinHeapReclaim(h);
    LocklessMinHeapDestroy(h);
}

TEST(LocklessMinHeapConcurrent, EqualKeyContentionStressesHelpingPath)
{
    /* Every key identical: delmin always targets the leftmost node, so many
     * consumers race on the mark/unlink CAS.  Exercise the cooperative
     * helping path and assert nothing is lost or double-extracted. */
    LocklessMinHeap *h = LocklessMinHeapCreate();
    const int P = 4, N = 5000;
    const int total = P * N;
    std::atomic<long> inserted{0}, extracted{0};
    std::vector<std::thread> th;

    for (int t = 0; t < P; ++t) {
        th.emplace_back([&] {
            for (int i = 0; i < N; ++i) {
                if (LocklessMinHeapInsertI64(h, 7, nullptr) == LOCKLESS_MINHEAP_OK)
                    inserted.fetch_add(1);
            }
        });
    }
    for (int c = 0; c < 2; ++c) {
        th.emplace_back([&] {
            for (;;) {
                if (LocklessMinHeapDelminI64(h, nullptr, nullptr)) {
                    extracted.fetch_add(1);
                } else if (extracted.load() == total) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto &x : th)
        x.join();

    while (LocklessMinHeapDelminI64(h, nullptr, nullptr))
        extracted.fetch_add(1);

    EXPECT_EQ(inserted.load(), (long)total);
    EXPECT_EQ(extracted.load(), (long)total);
    EXPECT_EQ(LocklessMinHeapSizeApprox(h), 0u);
    LocklessMinHeapReclaim(h);
    LocklessMinHeapDestroy(h);
}

/* ── Introspection ─────────────────────────────────────────────────────── */

TEST(LocklessMinHeapDescribe, EmitsStateJson)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    ASSERT_EQ(LocklessMinHeapInsertI64(h, 5, (void *)0x1234), LOCKLESS_MINHEAP_OK);
    ASSERT_EQ(LocklessMinHeapInsertI64(h, 3, nullptr), LOCKLESS_MINHEAP_OK);

    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 512);
    ASSERT_NE(bd.data, nullptr);
    EXPECT_EQ(DescribeLocklessMinHeap(h, &bd), &bd);

    EXPECT_NE(strstr(bd.data, "\"approx_size\":2"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"live_nodes\":2"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"key\":3"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"key\":5"), nullptr);
    BufferDescriptorRelease(&bd);
    LocklessMinHeapDestroy(h);
}

TEST(LocklessMinHeapDescribe, NullHandleEmitsError)
{
    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 128);
    ASSERT_NE(bd.data, nullptr);
    EXPECT_EQ(DescribeLocklessMinHeap(nullptr, &bd), &bd);
    EXPECT_NE(strstr(bd.data, "\"error\":\"null handle\""), nullptr);
    BufferDescriptorRelease(&bd);
}

TEST(LocklessMinHeapDescribe, AllocatesWhenProvidedNull)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    BufferDescriptor *out = DescribeLocklessMinHeap(h, nullptr);
    ASSERT_NE(out, nullptr);
    ASSERT_NE(out->data, nullptr);
    EXPECT_NE(strstr(out->data, "\"live_nodes\":0"), nullptr);
    BufferDescriptorRelease(out);
    free(out);
    LocklessMinHeapDestroy(h);
}

/* ── White-box layout invariants ───────────────────────────────────────── */

TEST(LocklessMinHeapLayout, NodeRetireIsFirstMember)
{
    /* container() relies on retire being at offset 0. */
    EXPECT_EQ(offsetof(struct LocklessMinHeapNode, retire), 0u);
    EXPECT_GE(sizeof(struct LocklessMinHeapNode), sizeof(int64_t) + sizeof(void *));
}

TEST(LocklessMinHeapLayout, ApproxSizeIsolatedOnOwnCacheLine)
{
    EXPECT_GE(offsetof(struct LocklessMinHeap, approx_size), 64u);
}

}  /* namespace */
