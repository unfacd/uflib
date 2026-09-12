/**
 * @file scheduled_jobs_adversarial_tests.cpp
 * @brief Adversarial gtest suite for the V1 scheduled_jobs module.
 *
 * Unlike the behavioural suite (scheduled_jobs_tests.cpp), this file
 * deliberately drives the scheduler with hostile inputs and concurrency in an
 * effort to break it:
 *
 *   - extreme / negative / duplicate fire-times,
 *   - huge capacity requests (degradation, not crash),
 *   - registration beyond the internal expansion threshold,
 *   - many threads inserting and removing concurrently, with "no lost job"
 *     and "drain is sorted" invariants checked at the end.
 *
 * Known robustness gaps (not covered here because they would abort the whole
 * process; they need a NULL-guard fix first):
 *   - every entry point dereferences `jobs_ptr` / `job_type_ptr` /
 *     `context_ptr_out` / `type_name` without a NULL check and will segfault.
 *   - `on_compare_keys` is declared on `ScheduledJobType` but never consulted
 *     by the store (the store orders by inline int64 fire-time).
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

extern "C" {
#include <uflib/scheduled_jobs/scheduled_jobs.h>
}

namespace {

/* ── Controllable fake clock (monotonic-ish; can go negative) ───────── */

static std::atomic<long long> g_clock{0};

extern "C" {
static long long sFakeGetTime(void)
{
    return g_clock.load(std::memory_order_relaxed);
}

static int sFakeOnRun(void * /*job*/, void * /*client*/) { return 0; }
}  /* extern "C" */

/* ── Fixture: a zeroed store, destroyed on teardown ──────────────────── */

class ScheduledJobsAdversarialTest : public ::testing::Test {
protected:
    ScheduledJobs store_;

    void SetUp() override
    {
        memset(&store_, 0, sizeof(store_));
        g_clock = 0;
    }

    void TearDown() override
    {
        DestructScheduledJobs(&store_);  /* NULL-safe: skips zeroed fields */
    }

    /* A type wired to the fake clock.  `on_compare_keys` is intentionally
     * left NULL — the store must not need it (i64 keys are ordered inline). */
    ScheduledJobType sMakeType(const char *name, uint64_t freq_us = 1000)
    {
        ScheduledJobType t;
        memset(&t, 0, sizeof(t));
        t.type_name       = name;
        t.frequency_mode  = PERIODIC;
        t.frequency       = freq_us;
        t.callbacks.on_get_time = sFakeGetTime;
        t.callbacks.on_run      = sFakeOnRun;
        return t;
    }

    /* A job whose fire time is `clock + when_to_schedule` (when_to_schedule
     * > 0 selects it over `frequency`).  With g_clock == 0 the fire time is
     * exactly `when_to_schedule`. */
    static ScheduledJob sMakeJob(ScheduledJobType *type, long long when)
    {
        ScheduledJob j;
        memset(&j, 0, sizeof(j));
        j.job_type_ptr     = type;
        j.when_to_schedule = when;
        return j;
    }
};

/* ── Robustness / degradation ────────────────────────────────────────── */

TEST_F(ScheduledJobsAdversarialTest, HugeCapacityRequestDegradesToNullStore)
{
    /* (int)SIZE_MAX == -1 → MinHeapCreateI64 returns NULL; the store must not
     * crash on subsequent ops — it degrades to "empty". */
    InitScheduledJobsStore(&store_, SIZE_MAX);

    ScheduledJobType t = sMakeType("huge_cap");
    ScheduledJob job = sMakeJob(&t, 100);

    InsertScheduledJob(&store_, &job);            /* must not crash */
    EXPECT_EQ(GetScheduleJobsSetsize(&store_, LOCK_HINT_NONE), 0u);

    ScheduledJobContext ctx{};
    EXPECT_EQ(GetScheduledJob(&store_, LOCK_HINT_NONE, &ctx), nullptr);
    EXPECT_EQ(GetRemScheduledJob(&store_, LOCK_HINT_NONE, &ctx), nullptr);
}

TEST_F(ScheduledJobsAdversarialTest, RepeatedInitDestructCycles)
{
    /* Many init/destruct cycles must not leak or corrupt state (run under
     * ASan/LSan to catch leaks). */
    ScheduledJobType t = sMakeType("cycle");
    for (int i = 0; i < 100; ++i) {
        InitScheduledJobsStore(&store_, 4);
        RegisterScheduledJobType(&store_, &t);
        ScheduledJob job = sMakeJob(&t, i + 1);
        InsertScheduledJob(&store_, &job);
        DestructScheduledJobs(&store_);
    }
    EXPECT_EQ(store_.scheduled_jobs_store, nullptr);
    EXPECT_EQ(store_.job_types_descriptor.job_types_index, nullptr);
}

/* ── Ordering invariants ─────────────────────────────────────────────── */

static std::vector<long long> sDrainAll(ScheduledJobs *s)
{
    std::vector<long long> times;
    for (;;) {
        ScheduledJobContext ctx{};
        if (!GetRemScheduledJob(s, LOCK_HINT_NONE, &ctx))
            break;
        times.push_back(ctx.time_key);
    }
    return times;
}

TEST_F(ScheduledJobsAdversarialTest, ExtractOrderIsSortedForRandomTimes)
{
    InitScheduledJobsStore(&store_, 8);
    ScheduledJobType t = sMakeType("order");
    RegisterScheduledJobType(&store_, &t);

    std::mt19937 rng(20260912);
    std::vector<ScheduledJob> jobs;
    jobs.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        long long when = static_cast<long long>(rng() % 100000000);
        jobs.push_back(sMakeJob(&t, when));
        InsertScheduledJob(&store_, &jobs.back());
    }

    auto times = sDrainAll(&store_);
    ASSERT_EQ(times.size(), 1000u);
    EXPECT_TRUE(std::is_sorted(times.begin(), times.end()));
}

TEST_F(ScheduledJobsAdversarialTest, ExtractOrderWithNegativeClock)
{
    /* Negative fire times: set the clock negative so fire time < 0. */
    InitScheduledJobsStore(&store_, 8);
    ScheduledJobType t = sMakeType("neg");
    RegisterScheduledJobType(&store_, &t);

    g_clock = -1000000;
    long long whens[] = {500000, 1000, 300000, 100000, 400000};
    std::vector<ScheduledJob> jobs;
    for (long long w : whens) {
        jobs.push_back(sMakeJob(&t, w));
        InsertScheduledJob(&store_, &jobs.back());
    }

    auto times = sDrainAll(&store_);
    EXPECT_TRUE(std::is_sorted(times.begin(), times.end()));
    EXPECT_LT(times.front(), 0);  /* all fire times are negative */
}

TEST_F(ScheduledJobsAdversarialTest, ExtractOrderWithDuplicateTimes)
{
    InitScheduledJobsStore(&store_, 8);
    ScheduledJobType t = sMakeType("dup");
    RegisterScheduledJobType(&store_, &t);

    std::vector<ScheduledJob> jobs;
    for (int i = 0; i < 100; ++i) {
        jobs.push_back(sMakeJob(&t, 42));  /* identical fire time */
        InsertScheduledJob(&store_, &jobs.back());
    }

    auto times = sDrainAll(&store_);
    ASSERT_EQ(times.size(), 100u);
    EXPECT_TRUE(std::is_sorted(times.begin(), times.end()));
}

TEST_F(ScheduledJobsAdversarialTest, ExtractOrderWithLargeTimes)
{
    InitScheduledJobsStore(&store_, 8);
    ScheduledJobType t = sMakeType("large");
    RegisterScheduledJobType(&store_, &t);

    const long long big = 9000000000000000000LL;  /* ~9e18, no overflow at 0 clock */
    std::vector<ScheduledJob> jobs;
    long long whens[] = {big, 1, big - 1, big / 2, 100};
    for (long long w : whens) {
        jobs.push_back(sMakeJob(&t, w));
        InsertScheduledJob(&store_, &jobs.back());
    }

    auto times = sDrainAll(&store_);
    EXPECT_TRUE(std::is_sorted(times.begin(), times.end()));
}

/* ── Re-insertion (periodic) ─────────────────────────────────────────── */

TEST_F(ScheduledJobsAdversarialTest, ReinsertMovesJobToBack)
{
    InitScheduledJobsStore(&store_, 8);
    ScheduledJobType t = sMakeType("reinsert");
    RegisterScheduledJobType(&store_, &t);

    /* Insert one "far future" job and one "soon" job. */
    ScheduledJob far = sMakeJob(&t, 1000000);
    ScheduledJob soon = sMakeJob(&t, 10);
    InsertScheduledJob(&store_, &far);
    InsertScheduledJob(&store_, &soon);

    /* Remove the soon job (fires at 10), then re-insert it far out. */
    ScheduledJobContext ctx{};
    ASSERT_NE(GetRemScheduledJob(&store_, LOCK_HINT_NONE, &ctx), nullptr);
    EXPECT_EQ(ctx.time_key, 10);

    soon.when_to_schedule = 2000000;
    ReInsertScheduledJob(&store_, &soon);

    auto times = sDrainAll(&store_);
    ASSERT_EQ(times.size(), 2u);
    EXPECT_EQ(times[0], 1000000);  /* far (original) fires first now */
    EXPECT_EQ(times[1], 2000000);  /* re-inserted soon job fires last */
}

/* ── Registration expansion ──────────────────────────────────────────── */

TEST_F(ScheduledJobsAdversarialTest, RegisterBeyondExpansionThreshold)
{
    InitScheduledJobsStore(&store_, 8);
    const int N = 40;  /* > JOB_INDEX_EXPANSION_THRESHOLD (10) */

    std::vector<ScheduledJobType> types;
    types.reserve(N);
    char name[32];
    for (int i = 0; i < N; ++i) {
        snprintf(name, sizeof(name), "type_%d", i);
        types.push_back(sMakeType(name));
        EXPECT_GE(RegisterScheduledJobType(&store_, &types.back()), 0);
    }

    pthread_spin_lock(&store_.spin_lock);
    for (int i = 0; i < N; ++i) {
        snprintf(name, sizeof(name), "type_%d", i);
        EXPECT_TRUE(IsJobTypeNameRegistered(&store_, name));
    }
    pthread_spin_unlock(&store_.spin_lock);
}

/* ── Concurrency: no lost jobs + sorted drain ────────────────────────── */

TEST_F(ScheduledJobsAdversarialTest, ConcurrentInsertNoLostJobsAndSorted)
{
    InitScheduledJobsStore(&store_, 8);
    ScheduledJobType t = sMakeType("conc");
    RegisterScheduledJobType(&store_, &t);

    const int kThreads = 8;
    const int kPerThread = 2000;
    std::atomic<long long> seq{0};
    std::atomic<long long> inserted{0};

    /* Jobs get a unique monotonic fire time from `seq`, so the drain order is
     * strictly increasing if and only if nothing is lost or reordered. */
    auto worker = [&]() {
        std::vector<ScheduledJob> jobs;
        jobs.reserve(kPerThread);
        for (int i = 0; i < kPerThread; ++i) {
            jobs.push_back(sMakeJob(&t, seq.fetch_add(1) + 1));
            InsertScheduledJob(&store_, &jobs.back());
            inserted.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(worker);
    for (auto &th : threads)
        th.join();

    EXPECT_EQ(GetScheduleJobsSetsize(&store_, LOCK_HINT_NONE),
              static_cast<size_t>(inserted.load()));

    auto times = sDrainAll(&store_);
    ASSERT_EQ(times.size(), static_cast<size_t>(kThreads * kPerThread));
    EXPECT_TRUE(std::is_sorted(times.begin(), times.end()));
}

TEST_F(ScheduledJobsAdversarialTest, ConcurrentMixedOpsFinalStateConsistent)
{
    InitScheduledJobsStore(&store_, 8);
    ScheduledJobType t = sMakeType("mixed");
    RegisterScheduledJobType(&store_, &t);

    const int kThreads = 8;
    const int kOpsPerThread = 5000;
    std::atomic<long long> inserted{0};
    std::atomic<long long> removed{0};

    auto worker = [&](unsigned seed) {
        std::vector<ScheduledJob> live;
        for (int i = 0; i < kOpsPerThread; ++i) {
            seed = seed * 1103515245u + 12345u;
            bool do_insert = ((seed >> 16) % 3) != 0;
            if (do_insert) {
                live.push_back(sMakeJob(&t, (seed & 0x7fffffff) + 1));
                InsertScheduledJob(&store_, &live.back());
                inserted.fetch_add(1);
            } else {
                ScheduledJobContext ctx{};
                if (GetRemScheduledJob(&store_, LOCK_HINT_NONE, &ctx))
                    removed.fetch_add(1);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(worker, 0xC0FFEEu ^ (unsigned)(i * 7919));
    for (auto &th : threads)
        th.join();

    /* Remaining jobs must be exactly (inserted - removed), and drain in
     * sorted order. */
    size_t remaining = GetScheduleJobsSetsize(&store_, LOCK_HINT_NONE);
    EXPECT_EQ(remaining,
              static_cast<size_t>(inserted.load() - removed.load()));

    auto times = sDrainAll(&store_);
    EXPECT_TRUE(std::is_sorted(times.begin(), times.end()));
}

/* ── Comparator helpers (public) ─────────────────────────────────────── */

TEST_F(ScheduledJobsAdversarialTest, TimeValueComparatorTreatsPointerAsValue)
{
    /* The comparator interprets void* as the integer value (bit-cast), not as
     * a pointer to a value.  NULL is value 0. */
    EXPECT_LT(TimeValueComparator((void *)0, (void *)100), 0);
    EXPECT_GT(TimeValueComparator((void *)200, (void *)100), 0);
    EXPECT_EQ(TimeValueComparator(nullptr, nullptr), 0);
    EXPECT_LT(TimeValueComparator(nullptr, (void *)1), 0);
}

TEST_F(ScheduledJobsAdversarialTest, DefaultComparatorIsCallable)
{
    CallbackOnCompareKeys cmp = GetDefaultComparatorForTimeValue();
    ASSERT_NE(cmp, nullptr);
    EXPECT_LT(cmp((void *)1, (void *)2), 0);
    EXPECT_GT(cmp((void *)2, (void *)1), 0);
    EXPECT_EQ(cmp((void *)7, (void *)7), 0);
}

/* ── DescribeScheduledJobs introspection ─────────────────────────────── */

TEST_F(ScheduledJobsAdversarialTest, DescribeEmitsStateJson)
{
    InitScheduledJobsStore(&store_, 8);
    ScheduledJobType t0 = sMakeType("alpha_type");
    ScheduledJobType t1 = sMakeType("beta_type", 2000);
    RegisterScheduledJobType(&store_, &t0);
    RegisterScheduledJobType(&store_, &t1);

    ScheduledJob j0 = sMakeJob(&t0, 100);
    ScheduledJob j1 = sMakeJob(&t1, 500);
    InsertScheduledJob(&store_, &j0);
    InsertScheduledJob(&store_, &j1);

    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 512);
    ASSERT_NE(bd.data, nullptr);
    EXPECT_EQ(DescribeScheduledJobs(&store_, &bd), &bd);

    EXPECT_NE(strstr(bd.data, "\"job_types_count\":2"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"pending_jobs\":2"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"type_name\":\"alpha_type\""), nullptr);
    EXPECT_NE(strstr(bd.data, "\"type_name\":\"beta_type\""), nullptr);
    EXPECT_NE(strstr(bd.data, "\"frequency_mode\":\"PERIODIC\""), nullptr);
    EXPECT_NE(strstr(bd.data, "\"fire_time\":100"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"fire_time\":500"), nullptr);
    BufferDescriptorRelease(&bd);
}

TEST_F(ScheduledJobsAdversarialTest, DescribeEmptyStore)
{
    InitScheduledJobsStore(&store_, 4);
    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 256);
    ASSERT_NE(bd.data, nullptr);
    DescribeScheduledJobs(&store_, &bd);
    EXPECT_NE(strstr(bd.data, "\"job_types_count\":0"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"pending_jobs\":0"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"job_types\":[]"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"jobs\":[]"), nullptr);
    BufferDescriptorRelease(&bd);
}

TEST_F(ScheduledJobsAdversarialTest, DescribeAllocatesWhenProvidedNull)
{
    InitScheduledJobsStore(&store_, 4);
    BufferDescriptor *out = DescribeScheduledJobs(&store_, nullptr);
    ASSERT_NE(out, nullptr);
    ASSERT_NE(out->data, nullptr);
    EXPECT_NE(strstr(out->data, "\"job_types_count\":0"), nullptr);
    BufferDescriptorRelease(out);
    free(out);
}

TEST_F(ScheduledJobsAdversarialTest, DescribeNullHandleEmitsError)
{
    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 128);
    ASSERT_NE(bd.data, nullptr);
    EXPECT_EQ(DescribeScheduledJobs(nullptr, &bd), &bd);
    EXPECT_NE(strstr(bd.data, "\"error\":\"null handle\""), nullptr);
    BufferDescriptorRelease(&bd);
}

}  /* namespace */
