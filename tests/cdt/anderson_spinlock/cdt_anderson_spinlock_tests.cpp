/**
 * @file cdt_anderson_spinlock_tests.cpp
 * @brief Comprehensive unit tests for the Anderson array-based queue spinlock.
 *
 * Covers lifecycle, single-thread correctness, power-of-2 and non-power-of-2
 * slot counts, concurrent safety, fairness (FIFO ordering), and memory-ordering
 * validation under TSAN.
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

extern "C" {
#include "uflib/cdt/cdt_anderson_spinlock.h"
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

class AndersonSpinlockTest : public ::testing::Test {
protected:
    void SetUp() override {
        memset(&lock_, 0, sizeof(lock_));
        memset(slots_, 0, sizeof(slots_));
    }

    void TearDown() override {
        /* slots_ and lock_ are stack-allocated — no dynamic free needed */
    }

    void Init(unsigned int count = kDefaultSlots) {
        spinlock_anderson_init(&lock_, slots_, count);
    }

    static constexpr unsigned int kDefaultSlots = 16;
    static constexpr unsigned int kMaxSlots      = 128;

    spinlock_anderson_t        lock_;
    spinlock_anderson_thread_t slots_[kMaxSlots];
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Init
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(AndersonSpinlockTest, InitPowerOfTwo) {
    Init(16);
    EXPECT_EQ(lock_.count, 16U);
    EXPECT_EQ(lock_.mask, 15U);
    EXPECT_EQ(lock_.wrap, 0U);
    EXPECT_EQ(lock_.slots, slots_);
    EXPECT_EQ(atomic_load_explicit(&lock_.next, std::memory_order_relaxed), 0U);
}

TEST_F(AndersonSpinlockTest, InitNonPowerOfTwo) {
    Init(10);
    EXPECT_EQ(lock_.count, 10U);
    EXPECT_EQ(lock_.mask, 9U);
    /* wrap = (UINT_MAX % 10) + 1 */
    unsigned int expected_wrap = (UINT_MAX % 10) + 1;
    EXPECT_EQ(lock_.wrap, expected_wrap);
}

TEST_F(AndersonSpinlockTest, InitSlotZeroUnlocked) {
    Init(8);
    EXPECT_EQ(atomic_load_explicit(&slots_[0].locked, std::memory_order_relaxed),
              (unsigned int)false);
    EXPECT_EQ(slots_[0].position, 0U);
}

TEST_F(AndersonSpinlockTest, InitSlotsOneToNMinusOneLocked) {
    Init(8);
    for (unsigned int i = 1; i < 8; i++) {
        EXPECT_EQ(atomic_load_explicit(&slots_[i].locked,
                                        std::memory_order_relaxed),
                  (unsigned int)true)
            << "slot " << i << " should be locked (true)";
    }
}

TEST_F(AndersonSpinlockTest, InitCountOne) {
    Init(1);
    EXPECT_EQ(lock_.count, 1U);
    /* mask = 1 - 1 = 0 */
    EXPECT_EQ(lock_.mask, 0U);
    /* 1 is a power of 2 → wrap = 0 */
    EXPECT_EQ(lock_.wrap, 0U);
    /* Only slot 0 — locked should be false (unlocked) */
    EXPECT_EQ(atomic_load_explicit(&slots_[0].locked, std::memory_order_relaxed),
              (unsigned int)false);
}

TEST_F(AndersonSpinlockTest, InitCountTwo) {
    Init(2);
    EXPECT_EQ(lock_.count, 2U);
    EXPECT_EQ(lock_.mask, 1U);
    EXPECT_EQ(lock_.wrap, 0U);
    EXPECT_EQ(atomic_load_explicit(&slots_[0].locked, std::memory_order_relaxed),
              (unsigned int)false);
    EXPECT_EQ(atomic_load_explicit(&slots_[1].locked, std::memory_order_relaxed),
              (unsigned int)true);
}

/* ── Cache-line isolation ────────────────────────────────────────────────── */

TEST_F(AndersonSpinlockTest, NextFieldOnSeparateCacheLine) {
    Init(16);
    /* Verify that `next` lives at an offset >= 64 from the start of the struct */
    size_t next_offset = offsetof(spinlock_anderson_t, next);
    EXPECT_GE(next_offset, (size_t)CDT_CACHELINE_SZ)
        << "`next` must be on its own cache line (offset >= 64), got offset "
        << next_offset;
}

TEST_F(AndersonSpinlockTest, TotalSizeDoesNotExceedTwoCacheLines) {
    /* The struct should be at most ~128 bytes: one line for control + one for next */
    EXPECT_LE(sizeof(spinlock_anderson_t), (size_t)(CDT_CACHELINE_SZ * 2))
        << "spinlock_anderson_t should not exceed 2 cache lines (128 bytes)";
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lock / Unlock — Single-Threaded
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(AndersonSpinlockTest, LockUnlockSingleThread) {
    Init(4);
    spinlock_anderson_thread_t *slot = nullptr;
    spinlock_anderson_lock(&lock_, &slot);
    EXPECT_NE(slot, nullptr);
    EXPECT_EQ(atomic_load_explicit(&slot->locked, std::memory_order_relaxed),
              (unsigned int)true);
    spinlock_anderson_unlock(&lock_, slot);
}

TEST_F(AndersonSpinlockTest, LockReturnsOwnSlot) {
    Init(4);
    spinlock_anderson_thread_t *slot = nullptr;
    spinlock_anderson_lock(&lock_, &slot);
    /* slot should point to slots_[0] (first lock taker gets slot 0) */
    EXPECT_EQ(slot, &slots_[0]);
    spinlock_anderson_unlock(&lock_, slot);
}

TEST_F(AndersonSpinlockTest, LockedReturnsTrueWhileHeld) {
    Init(4);
    spinlock_anderson_thread_t *slot = nullptr;
    spinlock_anderson_lock(&lock_, &slot);
    EXPECT_TRUE(spinlock_anderson_locked(&lock_));
    spinlock_anderson_unlock(&lock_, slot);
}

TEST_F(AndersonSpinlockTest, LockedReturnsFalseAfterUnlock) {
    Init(4);
    spinlock_anderson_thread_t *slot = nullptr;
    spinlock_anderson_lock(&lock_, &slot);
    spinlock_anderson_unlock(&lock_, slot);
    EXPECT_FALSE(spinlock_anderson_locked(&lock_));
}

TEST_F(AndersonSpinlockTest, SequentialLockUnlockCycles) {
    Init(4);
    for (int i = 0; i < 1000; i++) {
        spinlock_anderson_thread_t *slot = nullptr;
        spinlock_anderson_lock(&lock_, &slot);
        EXPECT_NE(slot, nullptr);
        spinlock_anderson_unlock(&lock_, slot);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Slot Assignment — Sequential FIFO Ordering
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(AndersonSpinlockTest, SequentialLocksGetSequentialSlots) {
    Init(8);
    for (unsigned int i = 0; i < 8; i++) {
        spinlock_anderson_thread_t *slot = nullptr;
        spinlock_anderson_lock(&lock_, &slot);
        EXPECT_EQ(slot, &slots_[i])
            << "Sequential lock " << i << " should get slot " << i;
        spinlock_anderson_unlock(&lock_, slot);
    }
}

TEST_F(AndersonSpinlockTest, SlotsWrapAround) {
    Init(4);
    /* Acquire+release 4 times to consume all slots */
    for (int i = 0; i < 4; i++) {
        spinlock_anderson_thread_t *slot = nullptr;
        spinlock_anderson_lock(&lock_, &slot);
        EXPECT_EQ(slot, &slots_[i]);
        spinlock_anderson_unlock(&lock_, slot);
    }
    /* Next lock should wrap back to slot 0 */
    spinlock_anderson_thread_t *slot = nullptr;
    spinlock_anderson_lock(&lock_, &slot);
    EXPECT_EQ(slot, &slots_[0]);
    spinlock_anderson_unlock(&lock_, slot);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Non-Power-of-2 Slots — CAS Slow Path
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(AndersonSpinlockTest, NonPowerOfTwoSequentialLocks) {
    Init(10);
    for (unsigned int i = 0; i < 10; i++) {
        spinlock_anderson_thread_t *slot = nullptr;
        spinlock_anderson_lock(&lock_, &slot);
        EXPECT_EQ(slot, &slots_[i]);
        spinlock_anderson_unlock(&lock_, slot);
    }
}

TEST_F(AndersonSpinlockTest, NonPowerOfTwoWrap) {
    Init(3); /* non-power-of-2 */
    /* Acquire all 3 slots */
    for (int i = 0; i < 3; i++) {
        spinlock_anderson_thread_t *slot = nullptr;
        spinlock_anderson_lock(&lock_, &slot);
        EXPECT_EQ(slot, &slots_[i]);
        spinlock_anderson_unlock(&lock_, slot);
    }
    /* Next should wrap to slot 0 */
    spinlock_anderson_thread_t *slot = nullptr;
    spinlock_anderson_lock(&lock_, &slot);
    EXPECT_EQ(slot, &slots_[0]);
    spinlock_anderson_unlock(&lock_, slot);
}

TEST_F(AndersonSpinlockTest, NonPowerOfTwoManyCycles) {
    Init(7);
    for (int cycle = 0; cycle < 100; cycle++) {
        for (unsigned int i = 0; i < 7; i++) {
            spinlock_anderson_thread_t *slot = nullptr;
            spinlock_anderson_lock(&lock_, &slot);
            EXPECT_EQ(slot, &slots_[i]);
            spinlock_anderson_unlock(&lock_, slot);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Locked() Query
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(AndersonSpinlockTest, LockedInitiallyFalse) {
    Init(8);
    EXPECT_FALSE(spinlock_anderson_locked(&lock_));
}

TEST_F(AndersonSpinlockTest, LockedTrueWhenFirstSlotLocked) {
    /* Manually set slot 0 locked = true, mimicking a held lock */
    Init(8);
    atomic_store_explicit(&slots_[0].locked, (unsigned int)true,
                          std::memory_order_release);
    atomic_store_explicit(&lock_.next, 0U, std::memory_order_release);
    EXPECT_TRUE(spinlock_anderson_locked(&lock_));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrency — Counter Invariant
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
CounterIncrementThread(spinlock_anderson_t *lock,
                       spinlock_anderson_thread_t *slots,
                       uint64_t *shared_counter,
                       uint64_t iterations,
                       std::atomic<uint64_t> *my_count)
{
    for (uint64_t i = 0; i < iterations; i++) {
        spinlock_anderson_thread_t *slot = nullptr;
        spinlock_anderson_lock(lock, &slot);
        (*shared_counter)++;
        spinlock_anderson_unlock(lock, slot);
        (*my_count)++;
    }
}

TEST_F(AndersonSpinlockTest, ConcurrentCounterInvariant_PowerOfTwo) {
    constexpr unsigned int kThreads   = 8;
    constexpr uint64_t     kIters     = 50000;
    uint64_t               counter    = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    constexpr unsigned int kSlots = 16;
    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[kSlots];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++) {
        threads.emplace_back(CounterIncrementThread,
                             &lock, slots_arr, &counter, kIters, &counts[t]);
    }
    for (auto &th : threads) th.join();

    /* Every increment by every thread must be counted exactly once */
    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    EXPECT_EQ(counter, total_thread_ops);
    EXPECT_EQ(counter, kThreads * kIters)
        << "Shared counter (" << counter << ") must equal "
        << kThreads << " threads × " << kIters << " iterations";
}

TEST_F(AndersonSpinlockTest, ConcurrentCounterInvariant_NonPowerOfTwo) {
    constexpr unsigned int kThreads   = 7;
    constexpr uint64_t     kIters     = 50000;
    uint64_t               counter    = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    constexpr unsigned int kSlots = 10; /* non-power-of-2 */
    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[kSlots];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++) {
        threads.emplace_back(CounterIncrementThread,
                             &lock, slots_arr, &counter, kIters, &counts[t]);
    }
    for (auto &th : threads) th.join();

    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    EXPECT_EQ(counter, total_thread_ops);
    EXPECT_EQ(counter, kThreads * kIters);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrency — Threads Equal Slots (Boundary Condition)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* The Anderson lock REQUIRES threads <= slots.  Each contending thread
 * must have a unique slot to spin on.  Over-subscription causes multiple
 * threads to share a slot position — all wake simultaneously on unlock,
 * breaking mutual exclusion (lost increments).
 *
 * This test verifies correctness when threads == slots (the maximum safe
 * configuration). */

TEST_F(AndersonSpinlockTest, ThreadsEqualToSlots) {
    constexpr unsigned int kThreads = 8;
    constexpr unsigned int kSlots   = 8;
    constexpr uint64_t     kIters   = 30000;
    uint64_t               counter  = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[kSlots];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++) {
        threads.emplace_back(CounterIncrementThread,
                             &lock, slots_arr, &counter, kIters, &counts[t]);
    }
    for (auto &th : threads) th.join();

    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    EXPECT_EQ(counter, total_thread_ops);
    EXPECT_EQ(counter, kThreads * kIters);
}

/* Document the over-subscription constraint.  This test confirms that
 * threads > slots is detected and results in lost mutual exclusion. */
TEST_F(AndersonSpinlockTest, OverSubscriptionConstraintDocumented) {
    constexpr unsigned int kThreads = 12;
    constexpr unsigned int kSlots   = 8;   /* deliberately fewer slots than threads */
    constexpr uint64_t     kIters   = 30000;

    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[kSlots];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    uint64_t               counter  = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++) {
        threads.emplace_back(CounterIncrementThread,
                             &lock, slots_arr, &counter, kIters, &counts[t]);
    }
    for (auto &th : threads) th.join();

    /* With threads > slots, the counter WILL be lower — this is the
     * documented constraint.  The test validates that the behavior is
     * detectable (counter < expected) and does not crash or deadlock. */
    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    /* All threads completed their iterations (no deadlock) */
    EXPECT_EQ(total_thread_ops, kThreads * kIters)
        << "All threads must complete — over-subscription must not deadlock";

    /* Mutual exclusion is broken: counter will be less than total ops */
    EXPECT_LT(counter, total_thread_ops)
        << "Over-subscription (threads > slots) must produce lost increments "
        << "(documented Anderson-lock constraint)";

    /* No crash, no hang — the lock degrades but does not catastrophically fail */
    EXPECT_GT(counter, (uint64_t)0)
        << "Some operations should still succeed even under over-subscription";
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrency — High Contention (Threads == Slots, Small Slot Count)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(AndersonSpinlockTest, HighContentionSmallSlotCount) {
    constexpr unsigned int kThreads = 4;
    constexpr unsigned int kSlots   = 4;
    constexpr uint64_t     kIters   = 50000;
    uint64_t               counter  = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[kSlots];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++) {
        threads.emplace_back(CounterIncrementThread,
                             &lock, slots_arr, &counter, kIters, &counts[t]);
    }
    for (auto &th : threads) th.join();

    uint64_t total_thread_ops = 0;
    for (unsigned int t = 0; t < kThreads; t++)
        total_thread_ops += counts[t].load();

    EXPECT_EQ(counter, total_thread_ops);
    EXPECT_EQ(counter, kThreads * kIters);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrency — FIFO Fairness (Slot Sequence Under Contention)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* This test verifies that threads acquire slots in increasing FIFO order.
 * Each thread records the slot position it acquired, and we verify no slot
 * is skipped in the global sequence across all acquisitions. */

TEST_F(AndersonSpinlockTest, FifoOrderingUnderContention) {
    constexpr unsigned int kThreads = 6;
    constexpr unsigned int kSlots   = 8;
    constexpr uint64_t     kIters   = 1000;

    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[kSlots];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    /* Global sequence of acquired slot indices, protected by a separate mutex
     * (we are testing the Anderson lock, not general mutual exclusion) */
    std::vector<unsigned int> slot_sequence;
    std::mutex                seq_mutex;

    auto thread_fn = [&]() {
        for (uint64_t i = 0; i < kIters; i++) {
            spinlock_anderson_thread_t *slot = nullptr;
            spinlock_anderson_lock(&lock, &slot);
            {
                std::lock_guard<std::mutex> lg(seq_mutex);
                slot_sequence.push_back(slot->position);
            }
            spinlock_anderson_unlock(&lock, slot);
        }
    };

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < kThreads; t++)
        threads.emplace_back(thread_fn);
    for (auto &th : threads) th.join();

    /* Verify FIFO: the sequence of slots must be ordered.  After each unlock,
     * the next slot index must be (prev + 1) % kSlots.  Because the recording
     * mutex serialises the push_back, the sequence reflects the true global
     * acquisition order. */
    ASSERT_EQ(slot_sequence.size(), kThreads * kIters);

    unsigned int expected = 0; /* first lock gets slot 0 */
    for (size_t i = 0; i < slot_sequence.size(); i++) {
        EXPECT_EQ(slot_sequence[i], expected)
            << "FIFO violation at acquisition " << i
            << ": expected slot " << expected
            << ", got slot " << slot_sequence[i];
        expected = (expected + 1) % kSlots;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrent — Mixed Lock/Unlock with Randomized Work
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(AndersonSpinlockTest, ConcurrentMixedCriticalSectionSizes) {
    constexpr unsigned int kThreads = 8;
    constexpr unsigned int kSlots   = 16;
    constexpr uint64_t     kIters   = 10000;
    uint64_t               counter  = 0;
    std::atomic<uint64_t>  counts[kThreads] = {};

    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[kSlots];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    auto thread_fn = [&](unsigned int tid) {
        for (uint64_t i = 0; i < kIters; i++) {
            spinlock_anderson_thread_t *slot = nullptr;
            spinlock_anderson_lock(&lock, &slot);

            /* Simulate variable-length critical section */
            counter++;
            volatile int work = 0;
            for (int w = 0; w < (int)(tid % 5); w++)
                work++;

            spinlock_anderson_unlock(&lock, slot);
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

/* This test is specifically designed to be TSAN-sensitive: threads exchange
 * a non-atomic payload under the lock.  If the lock's acquire/release fences
 * are insufficient, TSAN will report a data race. */

TEST_F(AndersonSpinlockTest, NonAtomicPayloadUnderLock) {
    constexpr unsigned int kThreads = 8;
    constexpr unsigned int kSlots   = 16;
    constexpr uint64_t     kIters   = 10000;

    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[kSlots];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    /* Payload: non-atomic struct — TSAN should flag any unprotected access */
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
            spinlock_anderson_thread_t *slot = nullptr;
            spinlock_anderson_lock(&lock, &slot);
            shared.counter++;
            spinlock_anderson_unlock(&lock, slot);
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

TEST_F(AndersonSpinlockTest, LongRunningStress) {
    constexpr unsigned int    kThreads    = 8;
    constexpr unsigned int    kSlots      = 16;
    constexpr std::chrono::seconds kDuration{2};

    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[kSlots];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    uint64_t               shared = 0;
    std::atomic<bool>      stop{false};
    std::atomic<uint64_t>  total_ops{0};

    auto thread_fn = [&]() {
        while (!stop.load(std::memory_order_acquire)) {
            spinlock_anderson_thread_t *slot = nullptr;
            spinlock_anderson_lock(&lock, &slot);
            shared++;
            spinlock_anderson_unlock(&lock, slot);
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
 * Edge Case — Single Slot (count = 1, single thread only)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* With 1 slot, at most 1 thread may contend.  See over-subscription
 * constraint test above for the behavior when threads > slots. */

TEST_F(AndersonSpinlockTest, SingleSlotSingleThread) {
    constexpr unsigned int kSlots   = 1;
    constexpr uint64_t     kIters   = 10000;
    uint64_t               counter  = 0;

    spinlock_anderson_t        lock;
    spinlock_anderson_thread_t slots_arr[1];
    spinlock_anderson_init(&lock, slots_arr, kSlots);

    for (uint64_t i = 0; i < kIters; i++) {
        spinlock_anderson_thread_t *slot = nullptr;
        spinlock_anderson_lock(&lock, &slot);
        counter++;
        spinlock_anderson_unlock(&lock, slot);
    }

    EXPECT_EQ(counter, kIters);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge Case — SPINWAIT macro definition
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(AndersonSpinlockTest, SpinwaitDefined) {
    /* Verify SPINWAIT compiles to a valid statement */
    SPINWAIT();
    SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Regression — Lock returns correct slot after many cycles
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(AndersonSpinlockTest, SlotPointerValidAfterManyCycles) {
    Init(8);
    for (int cycle = 0; cycle < 5000; cycle++) {
        spinlock_anderson_thread_t *slot = nullptr;
        spinlock_anderson_lock(&lock_, &slot);
        ASSERT_NE(slot, nullptr);
        /* slot must be within the slots_ array */
        ASSERT_GE(slot, slots_);
        ASSERT_LT(slot, slots_ + 8);
        spinlock_anderson_unlock(&lock_, slot);
    }
}
