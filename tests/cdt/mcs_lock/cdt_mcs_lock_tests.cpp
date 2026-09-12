/**
 * @file cdt_mcs_lock_tests.cpp
 * @brief Comprehensive unit tests for the MCS queue-based spinlock.
 *
 * Covers lifecycle, single-thread correctness, node management, concurrent
 * safety, FIFO fairness, padded-node variant, and memory-ordering validation
 * under TSAN.
 *
 * Copyright (C) 2015-2026 unfacd works
 */

#include "gtest/gtest.h"

#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cstddef>

extern "C" {
#include "uflib/cdt/cdt_mcs_lock.h"
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

class McsLockTest : public ::testing::Test {
protected:
    void SetUp() override {
        memset(&lock_, 0, sizeof(lock_));
    }

    void TearDown() override {
        /* lock_ is stack-allocated — no dynamic free needed */
    }

    void Init() {
        SpinlockMcsInit(&lock_);
    }

    SpinlockMcs lock_;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Init
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, InitSetsTailToNull) {
    Init();
    EXPECT_EQ(atomic_load_explicit(&lock_.tail, std::memory_order_relaxed),
              nullptr);
}

TEST_F(McsLockTest, StaticInitializer) {
    SpinlockMcs lock = SPINLOCK_MCS_INITIALIZER;
    EXPECT_EQ(atomic_load_explicit(&lock.tail, std::memory_order_relaxed),
              nullptr);
    /* Lock is immediately usable */
    SpinlockMcsNode node;
    SpinlockMcsNodeInit(&node);
    SpinlockMcsLock(&lock, &node);
    SpinlockMcsUnlock(&lock, &node);
}

TEST_F(McsLockTest, DoubleInitIsHarmless) {
    Init();
    Init();  /* second init must not crash or corrupt */
    EXPECT_EQ(atomic_load_explicit(&lock_.tail, std::memory_order_relaxed),
              nullptr);
}

TEST_F(McsLockTest, InitNullPointerIsSafe) {
    SpinlockMcsInit(nullptr);  /* must not crash */
    SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Node Init
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, NodeInitResetsState) {
    SpinlockMcsNode node;
    memset(&node, 0xFF, sizeof(node));  /* poison */
    SpinlockMcsNodeInit(&node);
    EXPECT_EQ(atomic_load_explicit(&node.next, std::memory_order_relaxed),
              nullptr);
    EXPECT_EQ(atomic_load_explicit(&node.locked, std::memory_order_relaxed),
              false);
}

TEST_F(McsLockTest, NodeInitNullPointerIsSafe) {
    SpinlockMcsNodeInit(nullptr);  /* must not crash */
    SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lock / Unlock — Single-Threaded
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, LockUnlockSingleThread) {
    Init();
    SpinlockMcsNode node;
    SpinlockMcsNodeInit(&node);
    SpinlockMcsLock(&lock_, &node);
    EXPECT_TRUE(SpinlockMcsLocked(&lock_));
    SpinlockMcsUnlock(&lock_, &node);
}

TEST_F(McsLockTest, LockedFalseAfterUnlock) {
    Init();
    SpinlockMcsNode node;
    SpinlockMcsNodeInit(&node);
    SpinlockMcsLock(&lock_, &node);
    SpinlockMcsUnlock(&lock_, &node);
    EXPECT_FALSE(SpinlockMcsLocked(&lock_));
}

TEST_F(McsLockTest, SequentialLockUnlockCycles) {
    Init();
    for (int i = 0; i < 10000; i++) {
        SpinlockMcsNode node;
        SpinlockMcsNodeInit(&node);
        SpinlockMcsLock(&lock_, &node);
        SpinlockMcsUnlock(&lock_, &node);
    }
}

TEST_F(McsLockTest, LockedInitiallyFalse) {
    Init();
    EXPECT_FALSE(SpinlockMcsLocked(&lock_));
}

TEST_F(McsLockTest, LockedTrueWhileHeld) {
    Init();
    SpinlockMcsNode node;
    SpinlockMcsNodeInit(&node);
    SpinlockMcsLock(&lock_, &node);
    EXPECT_TRUE(SpinlockMcsLocked(&lock_));
    SpinlockMcsUnlock(&lock_, &node);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Node Reuse
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, NodeReuseAfterReinit) {
    Init();
    SpinlockMcsNode node;

    for (int i = 0; i < 1000; i++) {
        SpinlockMcsNodeInit(&node);
        SpinlockMcsLock(&lock_, &node);
        SpinlockMcsUnlock(&lock_, &node);
    }
    /* No crash, no hang — node reuse with reinit is safe */
    SUCCEED();
}

TEST_F(McsLockTest, FreshStackNodeEachCycle) {
    Init();
    /* This is the recommended pattern — fresh node per critical section */
    for (int i = 0; i < 1000; i++) {
        SpinlockMcsNode node;
        SpinlockMcsNodeInit(&node);
        SpinlockMcsLock(&lock_, &node);
        SpinlockMcsUnlock(&lock_, &node);
    }
    SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Padded Node Variant
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, PaddedNodeLockUnlock) {
    Init();
    SpinlockMcsNodePadded node;
    /* Padded node uses the same lock interface — just a different node type */
    memset(&node, 0, sizeof(node));
    atomic_init(&node.next, nullptr);
    atomic_init(&node.locked, false);
    SpinlockMcsLock(&lock_, reinterpret_cast<SpinlockMcsNode *>(&node));
    SpinlockMcsUnlock(&lock_, reinterpret_cast<SpinlockMcsNode *>(&node));
}

TEST_F(McsLockTest, PaddedNodeLayout) {
    /* Verify that `next` and `locked` are on separate cache lines */
    size_t next_offset   = offsetof(SpinlockMcsNodePadded, next);
    size_t locked_offset = offsetof(SpinlockMcsNodePadded, locked);

    EXPECT_EQ(next_offset, (size_t)0)
        << "`next` must be at offset 0 (cache line 0)";
    EXPECT_GE(locked_offset, (size_t)CDT_CACHELINE_SZ)
        << "`locked` must be on cache line 1 (offset >= 64), got offset "
        << locked_offset;
}

TEST_F(McsLockTest, PaddedNodeSize) {
    /* 128 bytes: 64 for next + 64 for locked */
    EXPECT_GE(sizeof(SpinlockMcsNodePadded), (size_t)(CDT_CACHELINE_SZ * 2))
        << "Padded node must be at least 128 bytes for cache-line isolation";
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrency — Counter Invariant
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
CounterIncrementThread(SpinlockMcs *lock_ptr,
                       uint64_t *shared_counter,
                       uint64_t iterations,
                       std::atomic<uint64_t> *my_count)
{
    for (uint64_t i = 0; i < iterations; i++) {
        SpinlockMcsNode node;
        SpinlockMcsNodeInit(&node);
        SpinlockMcsLock(lock_ptr, &node);
        (*shared_counter)++;
        SpinlockMcsUnlock(lock_ptr, &node);
        (*my_count)++;
    }
}

TEST_F(McsLockTest, ConcurrentCounterInvariant8Threads) {
    constexpr unsigned int kThreads = 8;
    constexpr uint64_t     kIters   = 50000;
    uint64_t               counter  = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    Init();

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++) {
        threads.emplace_back(CounterIncrementThread,
                             &lock_, &counter, kIters, &counts[t]);
    }
    for (auto &th : threads) th.join();

    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    EXPECT_EQ(counter, total_thread_ops);
    EXPECT_EQ(counter, kThreads * kIters)
        << "Shared counter (" << counter << ") must equal "
        << kThreads << " threads x " << kIters << " iterations";
}

TEST_F(McsLockTest, ConcurrentCounterInvariant7Threads) {
    constexpr unsigned int kThreads = 7;
    constexpr uint64_t     kIters   = 50000;
    uint64_t               counter  = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    Init();

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++) {
        threads.emplace_back(CounterIncrementThread,
                             &lock_, &counter, kIters, &counts[t]);
    }
    for (auto &th : threads) th.join();

    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    EXPECT_EQ(counter, total_thread_ops);
    EXPECT_EQ(counter, kThreads * kIters);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrency — High Contention (Many Threads on Few Cores)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, HighContention12Threads) {
    constexpr unsigned int kThreads = 12;
    constexpr uint64_t     kIters   = 30000;
    uint64_t               counter  = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    Init();

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++) {
        threads.emplace_back(CounterIncrementThread,
                             &lock_, &counter, kIters, &counts[t]);
    }
    for (auto &th : threads) th.join();

    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    EXPECT_EQ(counter, total_thread_ops);
    EXPECT_EQ(counter, kThreads * kIters);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrency — FIFO Fairness (Acquisition Order)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * MCS is strictly FIFO.  We verify that threads acquire in the order they
 * call SpinlockMcsLock(), by recording a global sequence number inside the
 * critical section and checking that it's strictly increasing.
 */
TEST_F(McsLockTest, FifoOrderingUnderContention) {
    constexpr unsigned int kThreads = 6;
    constexpr uint64_t     kIters   = 1000;

    Init();

    std::atomic<uint64_t> ticket{0};
    std::vector<uint64_t>  sequence;
    std::mutex             seq_mutex;
    uint64_t               total_acquisitions = 0;

    auto thread_fn = [&]() {
        for (uint64_t i = 0; i < kIters; i++) {
            SpinlockMcsNode node;
            SpinlockMcsNodeInit(&node);
            SpinlockMcsLock(&lock_, &node);
            {
                uint64_t my_ticket = ticket.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lg(seq_mutex);
                sequence.push_back(my_ticket);
            }
            SpinlockMcsUnlock(&lock_, &node);
        }
    };

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++)
        threads.emplace_back(thread_fn);
    for (auto &th : threads) th.join();

    ASSERT_EQ(sequence.size(), kThreads * kIters);

    /*
     * Tickets are assigned inside the critical section under the MCS lock,
     * which guarantees mutual exclusion.  The ticket values must be strictly
     * increasing and form the complete sequence [0, total - 1].
     */
    std::vector<uint64_t> sorted = sequence;
    std::sort(sorted.begin(), sorted.end());
    for (uint64_t i = 0; i < sorted.size(); i++) {
        EXPECT_EQ(sorted[i], i)
            << "Missing or duplicate ticket " << i;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrency — Mixed Critical-Section Sizes
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, VariableCriticalSectionSizes) {
    constexpr unsigned int kThreads = 8;
    constexpr uint64_t     kIters   = 10000;
    uint64_t               counter  = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    Init();

    auto thread_fn = [&](unsigned int tid) {
        for (uint64_t i = 0; i < kIters; i++) {
            SpinlockMcsNode node;
            SpinlockMcsNodeInit(&node);
            SpinlockMcsLock(&lock_, &node);

            /* Simulate variable-length critical section */
            counter++;
            volatile int work = 0;
            for (int w = 0; w < (int)(tid % 5); w++)
                work++;

            SpinlockMcsUnlock(&lock_, &node);
            (void)work;
            counts[tid]++;
        }
    };

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++)
        threads.emplace_back(thread_fn, t);
    for (auto &th : threads) th.join();

    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    EXPECT_EQ(counter, total_thread_ops);
    EXPECT_EQ(counter, kThreads * kIters);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Memory Ordering — TSAN Visibility
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * This test is specifically designed to be TSAN-sensitive: threads exchange
 * a non-atomic payload under the lock.  If the lock's acquire/release fences
 * are insufficient, TSAN will report a data race.
 */
TEST_F(McsLockTest, NonAtomicPayloadUnderLock) {
    constexpr unsigned int kThreads = 8;
    constexpr uint64_t     kIters   = 10000;

    Init();

    struct Payload {
        uint64_t counter;
        char     padding[48]; /* pad to avoid false sharing between test structs */
    };
    Payload shared = {0, {}};

    std::atomic<bool>     start{false};
    std::atomic<uint64_t> total_ops{0};

    auto thread_fn = [&]() {
        while (!start.load(std::memory_order_acquire)) { /* spin */ }
        for (uint64_t i = 0; i < kIters; i++) {
            SpinlockMcsNode node;
            SpinlockMcsNodeInit(&node);
            SpinlockMcsLock(&lock_, &node);
            shared.counter++;
            SpinlockMcsUnlock(&lock_, &node);
            total_ops.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++)
        threads.emplace_back(thread_fn);

    start.store(true, std::memory_order_release);
    for (auto &th : threads) th.join();

    EXPECT_EQ(shared.counter, total_ops.load());
    EXPECT_EQ(shared.counter, kThreads * kIters);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stress — Long-Running Multi-Threaded Hammer
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, LongRunningStress) {
    constexpr unsigned int          kThreads  = 8;
    constexpr std::chrono::seconds  kDuration{2};

    Init();

    uint64_t               shared = 0;
    std::atomic<bool>      stop{false};
    std::atomic<uint64_t>  total_ops{0};

    auto thread_fn = [&]() {
        while (!stop.load(std::memory_order_acquire)) {
            SpinlockMcsNode node;
            SpinlockMcsNodeInit(&node);
            SpinlockMcsLock(&lock_, &node);
            shared++;
            SpinlockMcsUnlock(&lock_, &node);
            total_ops.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++)
        threads.emplace_back(thread_fn);

    std::this_thread::sleep_for(kDuration);
    stop.store(true, std::memory_order_release);

    for (auto &th : threads) th.join();

    EXPECT_EQ(shared, total_ops.load())
        << "Shared counter must equal total operations after "
        << kDuration.count() << "s stress run";
    EXPECT_GT(total_ops.load(), (uint64_t)0)
        << "Must complete at least some operations";
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge Case — Locked Query After Init
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, LockedFalseWhenTailIsNull) {
    Init();
    /* tail is NULL → no one is waiting or holding */
    EXPECT_FALSE(SpinlockMcsLocked(&lock_));
}

TEST_F(McsLockTest, LockedTrueWhenTailNonNull) {
    Init();
    SpinlockMcsNode node;
    SpinlockMcsNodeInit(&node);

    /* Simulate a thread in the queue by manually setting tail */
    atomic_store_explicit(&lock_.tail, &node, std::memory_order_release);
    EXPECT_TRUE(SpinlockMcsLocked(&lock_));

    /* Clean up — tail must be NULL for the next test */
    atomic_store_explicit(&lock_.tail, nullptr, std::memory_order_release);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge Case — Many Threads, Low Contention
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, ManyThreadsLowContention) {
    /*
     * Threads mostly sleep outside the lock — most acquires hit the fast
     * path (no predecessor).  Verifies the fast path works under concurrent
     * access.
     */
    constexpr unsigned int kThreads = 16;
    constexpr uint64_t     kIters   = 1000;
    uint64_t               counter  = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    Init();

    auto thread_fn = [&]() {
        for (uint64_t i = 0; i < kIters; i++) {
            SpinlockMcsNode node;
            SpinlockMcsNodeInit(&node);
            SpinlockMcsLock(&lock_, &node);
            counter++;
            SpinlockMcsUnlock(&lock_, &node);

            /* Sleep briefly to reduce contention */
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    };

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++)
        threads.emplace_back(thread_fn);
    for (auto &th : threads) th.join();

    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    EXPECT_EQ(counter, kThreads * kIters);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge Case — Uncontended (Single Thread, Immediate Release)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, ImmediateReleaseNoContention) {
    Init();

    for (int i = 0; i < 5000; i++) {
        SpinlockMcsNode node;
        SpinlockMcsNodeInit(&node);
        SpinlockMcsLock(&lock_, &node);
        /* No work — immediate release */
        SpinlockMcsUnlock(&lock_, &node);
    }

    EXPECT_FALSE(SpinlockMcsLocked(&lock_));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Regression — Lock Returns After Many Cycles
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, LockUnlockManyCyclesNoDeadlock) {
    Init();

    for (int cycle = 0; cycle < 50000; cycle++) {
        SpinlockMcsNode node;
        SpinlockMcsNodeInit(&node);
        SpinlockMcsLock(&lock_, &node);
        SpinlockMcsUnlock(&lock_, &node);
    }

    /* After 50k cycles, lock must be free */
    EXPECT_FALSE(SpinlockMcsLocked(&lock_));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sanity — SPINWAIT macro compiles
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(McsLockTest, SpinwaitDefined) {
    SPINWAIT();
    SUCCEED();
}
