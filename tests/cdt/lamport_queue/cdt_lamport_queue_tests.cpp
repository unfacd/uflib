/**
 * @file cdt_lamport_queue_tests.cpp
 * @brief Comprehensive unit tests for LamportQueue (LocklessSpscQueue).
 *
 * Covers lifecycle, single-threaded correctness, edge cases, and SPSC
 * concurrent safety.
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
#include "uflib/cdt/cdt_lamport_queue.h"
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

class LamportQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        memset(&queue_, 0, sizeof(queue_));
    }

    void TearDown() override {
        LamportQueueDestroy(&queue_);
    }

    void Init(size_t sz = 64) {
        LamportQueueInit(&queue_, NULL, sz);
    }

    void InitWithPayload(QueueClientData **payload, size_t sz) {
        LamportQueueInit(&queue_, payload, sz);
    }

    LocklessSpscQueue queue_;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Init / Destroy
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(LamportQueueTest, InitValid) {
    Init(64);
    EXPECT_EQ(queue_.queue_sz, 64U);
    EXPECT_EQ(atomic_load_explicit(&queue_.front_, std::memory_order_relaxed), 0U);
    EXPECT_EQ(atomic_load_explicit(&queue_.back_, std::memory_order_relaxed), 0U);
    EXPECT_EQ(atomic_load_explicit(&queue_.leased, std::memory_order_relaxed), 0U);
    EXPECT_NE(queue_.payload, nullptr);
    EXPECT_TRUE(queue_.owns_payload);
}

TEST_F(LamportQueueTest, InitZeroSizeClampedToTwo) {
    Init(0);
    /* queue_sz < 2 is clamped to 2 — minimum viable: 1 usable slot */
    EXPECT_EQ(queue_.queue_sz, 2U);
    EXPECT_NE(queue_.payload, nullptr);

    /* 1 slot usable — push one succeeds, second push fails (full) */
    QueueClientData *item = AS_QUEUE_CLIENT_DATA(0x1);
    EXPECT_TRUE(LamportQueuePush(&queue_, item));

    QueueClientData *item2 = AS_QUEUE_CLIENT_DATA(0x2);
    EXPECT_FALSE(LamportQueuePush(&queue_, item2));
}

TEST_F(LamportQueueTest, InitOneSizeClampedToTwo) {
    Init(1);
    /* Same as zero — clamped to 2 */
    EXPECT_EQ(queue_.queue_sz, 2U);
    EXPECT_NE(queue_.payload, nullptr);
}

TEST_F(LamportQueueTest, InitWithPreallocatedPayload) {
    QueueClientData *arr[8];
    memset(arr, 0, sizeof(arr));
    InitWithPayload(arr, 8);
    EXPECT_EQ(queue_.payload, arr);
    EXPECT_EQ(queue_.queue_sz, 8U);
    EXPECT_FALSE(queue_.owns_payload);
}

TEST_F(LamportQueueTest, InitWithNullPayloadCallocs) {
    Init(32);
    EXPECT_NE(queue_.payload, nullptr);
    EXPECT_TRUE(queue_.owns_payload);
    /* Verify the payload array is zeroed (calloc) */
    for (size_t i = 0; i < 32; i++) {
        EXPECT_EQ(queue_.payload[i], nullptr);
    }
}

TEST_F(LamportQueueTest, InitNullQueueIsSafe) {
    /* Must not crash */
    LamportQueueInit(NULL, NULL, 64);
    SUCCEED();
}

TEST_F(LamportQueueTest, DestroyFreesInternalPayload) {
    Init(16);
    void *saved_payload = queue_.payload;
    EXPECT_NE(saved_payload, nullptr);
    EXPECT_TRUE(queue_.owns_payload);

    LamportQueueDestroy(&queue_);
    /* After destroy, struct is zeroed */
    EXPECT_EQ(queue_.payload, nullptr);
    EXPECT_FALSE(queue_.owns_payload);
    EXPECT_EQ(queue_.queue_sz, 0U);
}

TEST_F(LamportQueueTest, DestroyDoesNotFreeExternalPayload) {
    QueueClientData *arr[4] = {};
    InitWithPayload(arr, 4);
    EXPECT_FALSE(queue_.owns_payload);

    LamportQueueDestroy(&queue_);
    /* External payload untouched — we still own arr[] */
    EXPECT_EQ(queue_.payload, nullptr);
    EXPECT_FALSE(queue_.owns_payload);
}

TEST_F(LamportQueueTest, DestroyIsIdempotent) {
    Init(8);
    LamportQueueDestroy(&queue_);
    LamportQueueDestroy(&queue_);  /* must not double-free or crash */
    EXPECT_EQ(queue_.payload, nullptr);
}

TEST_F(LamportQueueTest, DestroyNullIsSafe) {
    LamportQueueDestroy(NULL);  /* must not crash */
    SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Push / Pop — basic round-trip
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(LamportQueueTest, PushPopSingle) {
    Init(8);
    QueueClientData *sent = AS_QUEUE_CLIENT_DATA(0xDEADBEEF);
    EXPECT_TRUE(LamportQueuePush(&queue_, sent));

    QueueClientData *recv = NULL;
    EXPECT_TRUE(LamportQueuePop(&queue_, &recv));
    EXPECT_EQ(recv, sent);
}

TEST_F(LamportQueueTest, PushPopMultiple) {
    Init(64);
    constexpr int kN = 50;

    for (int i = 0; i < kN; i++) {
        QueueClientData *p = AS_QUEUE_CLIENT_DATA((uintptr_t)(i + 1000));
        EXPECT_TRUE(LamportQueuePush(&queue_, p));
    }

    /* Verify FIFO order */
    for (int i = 0; i < kN; i++) {
        QueueClientData *p = NULL;
        EXPECT_TRUE(LamportQueuePop(&queue_, &p));
        EXPECT_EQ((uintptr_t)p, (uintptr_t)(i + 1000))
            << "FIFO order violation at index " << i;
    }
}

TEST_F(LamportQueueTest, PopEmptyReturnsFalse) {
    Init(8);
    QueueClientData *recv = AS_QUEUE_CLIENT_DATA(0xFFFF);  /* sentinel */
    EXPECT_FALSE(LamportQueuePop(&queue_, &recv));
    /* recv must be unmodified on failure */
    EXPECT_EQ(recv, AS_QUEUE_CLIENT_DATA(0xFFFF));
}

TEST_F(LamportQueueTest, PushFullReturnsFalse) {
    Init(8);  /* 7 usable slots */
    QueueClientData *item = AS_QUEUE_CLIENT_DATA(0x1);

    /* Fill to capacity */
    for (int i = 0; i < 7; i++) {
        EXPECT_TRUE(LamportQueuePush(&queue_, item));
    }
    /* 8th push must fail */
    EXPECT_FALSE(LamportQueuePush(&queue_, item));
}

TEST_F(LamportQueueTest, PushPopWraparound) {
    Init(8);  /* 7 usable slots */

    /* Fill completely */
    for (int i = 0; i < 7; i++) {
        EXPECT_TRUE(LamportQueuePush(
            &queue_, AS_QUEUE_CLIENT_DATA((uintptr_t)(i + 1))));
    }
    /* Partial drain */
    for (int i = 0; i < 3; i++) {
        QueueClientData *p = NULL;
        EXPECT_TRUE(LamportQueuePop(&queue_, &p));
        EXPECT_EQ((uintptr_t)p, (uintptr_t)(i + 1));
    }
    /* Refill (exercises index wraparound) */
    for (int i = 7; i < 10; i++) {
        EXPECT_TRUE(LamportQueuePush(
            &queue_, AS_QUEUE_CLIENT_DATA((uintptr_t)(i + 1))));
    }
    /* Drain all remaining */
    for (int i = 3; i < 10; i++) {
        QueueClientData *p = NULL;
        EXPECT_TRUE(LamportQueuePop(&queue_, &p));
        EXPECT_EQ((uintptr_t)p, (uintptr_t)(i + 1))
            << "Wraparound FIFO violation at index " << i;
    }

    /* Queue should be empty now */
    QueueClientData *dummy = NULL;
    EXPECT_FALSE(LamportQueuePop(&queue_, &dummy));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FIFO ordering
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(LamportQueueTest, FifoOrderPreserved) {
    Init(16);

    QueueClientData *a = AS_QUEUE_CLIENT_DATA(0xA);
    QueueClientData *b = AS_QUEUE_CLIENT_DATA(0xB);
    QueueClientData *c = AS_QUEUE_CLIENT_DATA(0xC);

    EXPECT_TRUE(LamportQueuePush(&queue_, a));
    EXPECT_TRUE(LamportQueuePush(&queue_, b));
    EXPECT_TRUE(LamportQueuePush(&queue_, c));

    QueueClientData *p = NULL;
    EXPECT_TRUE(LamportQueuePop(&queue_, &p));
    EXPECT_EQ(p, a);
    EXPECT_TRUE(LamportQueuePop(&queue_, &p));
    EXPECT_EQ(p, b);
    EXPECT_TRUE(LamportQueuePop(&queue_, &p));
    EXPECT_EQ(p, c);
}

TEST_F(LamportQueueTest, FifoAfterWraparound) {
    Init(4);  /* 3 usable slots */

    /* Push 3, pop 3 — one full cycle */
    for (int cycle = 0; cycle < 20; cycle++) {
        for (int i = 0; i < 3; i++) {
            EXPECT_TRUE(LamportQueuePush(
                &queue_, AS_QUEUE_CLIENT_DATA((uintptr_t)(cycle * 3 + i))));
        }
        for (int i = 0; i < 3; i++) {
            QueueClientData *p = NULL;
            EXPECT_TRUE(LamportQueuePop(&queue_, &p));
            EXPECT_EQ((uintptr_t)p, (uintptr_t)(cycle * 3 + i))
                << "FIFO violation at cycle " << cycle << " index " << i;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Push / Pop — large volume (sequential)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(LamportQueueTest, PushPopManySequential) {
    Init(1024);
    constexpr int kN = 50000;

    for (int i = 0; i < kN; i++) {
        EXPECT_TRUE(LamportQueuePush(
            &queue_, AS_QUEUE_CLIENT_DATA((uintptr_t)i)));
        QueueClientData *p = NULL;
        EXPECT_TRUE(LamportQueuePop(&queue_, &p));
        EXPECT_EQ((uintptr_t)p, (uintptr_t)i)
            << "Round-trip mismatch at " << i;
    }
}

TEST_F(LamportQueueTest, PushPopBatchedVerifyAll) {
    Init(256);
    constexpr int kN = 100;
    bool seen[100] = {false};

    for (int i = 0; i < kN; i++) {
        EXPECT_TRUE(LamportQueuePush(
            &queue_, AS_QUEUE_CLIENT_DATA((uintptr_t)(i + 1))));
    }

    for (int i = 0; i < kN; i++) {
        QueueClientData *p = NULL;
        EXPECT_TRUE(LamportQueuePop(&queue_, &p));
        uintptr_t id = (uintptr_t)p;
        EXPECT_GE(id, 1U);
        EXPECT_LE(id, (uintptr_t)kN);
        EXPECT_FALSE(seen[id - 1]) << "Duplicate item: " << id;
        seen[id - 1] = true;
    }

    for (int i = 0; i < kN; i++) {
        EXPECT_TRUE(seen[i]) << "Missing item: " << (i + 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Full / Empty boundary conditions
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(LamportQueueTest, EmptyAfterInit) {
    Init(8);
    QueueClientData *p = NULL;
    EXPECT_FALSE(LamportQueuePop(&queue_, &p));
}

TEST_F(LamportQueueTest, FullAfterFill) {
    Init(8);
    for (int i = 0; i < 7; i++) {
        EXPECT_TRUE(LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA(0x1)));
    }
    EXPECT_FALSE(LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA(0x1)));
}

TEST_F(LamportQueueTest, NotEmptyAfterPush) {
    Init(8);
    LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA(0x1));
    QueueClientData *p = NULL;
    EXPECT_TRUE(LamportQueuePop(&queue_, &p));
}

TEST_F(LamportQueueTest, NotFullAfterPop) {
    Init(8);
    /* Fill */
    for (int i = 0; i < 7; i++) {
        LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA(0x1));
    }
    EXPECT_FALSE(LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA(0x1)));  /* full */

    /* Pop one */
    QueueClientData *p = NULL;
    EXPECT_TRUE(LamportQueuePop(&queue_, &p));

    /* Now one slot is free */
    EXPECT_TRUE(LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA(0x2)));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge cases / NULL guards
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(LamportQueueTest, PushNullQueueReturnsFalse) {
    EXPECT_FALSE(LamportQueuePush(NULL, AS_QUEUE_CLIENT_DATA(0x1)));
}

TEST_F(LamportQueueTest, PopNullQueueReturnsFalse) {
    QueueClientData *p = NULL;
    EXPECT_FALSE(LamportQueuePop(NULL, &p));
}

TEST_F(LamportQueueTest, PopNullElemReturnsFalse) {
    Init(8);
    LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA(0x1));
    EXPECT_FALSE(LamportQueuePop(&queue_, NULL));
}

TEST_F(LamportQueueTest, LeasedSizeNullQueueReturnsZero) {
    EXPECT_EQ(LamportQueueLeasedSize(NULL), 0U);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LeasedSize
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(LamportQueueTest, LeasedSizeStartsAtZero) {
    Init(64);
    EXPECT_EQ(LamportQueueLeasedSize(&queue_), 0U);
}

TEST_F(LamportQueueTest, LeasedSizeTracksPushPop) {
    Init(64);

    EXPECT_EQ(LamportQueueLeasedSize(&queue_), 0U);

    for (int i = 0; i < 5; i++) {
        LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA((uintptr_t)i));
    }
    /* After 5 pushes: leased should be 5 (or at least close — snapshot) */
    size_t leased = LamportQueueLeasedSize(&queue_);
    EXPECT_EQ(leased, 5U);

    QueueClientData *p = NULL;
    LamportQueuePop(&queue_, &p);
    LamportQueuePop(&queue_, &p);
    EXPECT_EQ(LamportQueueLeasedSize(&queue_), 3U);

    /* Pop remaining */
    for (int i = 0; i < 3; i++) {
        LamportQueuePop(&queue_, &p);
    }
    EXPECT_EQ(LamportQueueLeasedSize(&queue_), 0U);
}

TEST_F(LamportQueueTest, LeasedSizeNeverExceedsCapacity) {
    Init(64);  /* 63 usable */
    for (int i = 0; i < 63; i++) {
        LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA((uintptr_t)i));
    }
    /* Leased should never exceed usable capacity */
    size_t leased = LamportQueueLeasedSize(&queue_);
    EXPECT_LE(leased, 63U);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrent: SPSC — producer pushes, consumer pops
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(LamportQueueTest, SpscConcurrentPushPop) {
    Init(65536);
    constexpr uint64_t kNItems = 500000;

    std::atomic<uint64_t> push_count{0};
    std::atomic<uint64_t> pop_count{0};
    std::atomic<uint64_t> push_errors{0};
    std::atomic<uint64_t> pop_errors{0};
    std::atomic<bool> start{false};

    /* Producer */
    std::thread producer([this, &start, &push_count, &push_errors]() {
        while (!start.load()) { /* spin */ }
        for (uint64_t i = 0; i < kNItems; i++) {
            if (LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA((uintptr_t)(i + 1)))) {
                push_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                push_errors.fetch_add(1, std::memory_order_relaxed);
                /* Spin until consumer frees a slot */
                i--;
            }
        }
    });

    /* Consumer — verify monotonic, gap-free sequence */
    std::thread consumer([this, &start, &pop_count, &pop_errors]() {
        uint64_t expected = 1;
        while (!start.load()) { /* spin */ }
        while (pop_count.load(std::memory_order_relaxed) < kNItems) {
            QueueClientData *p = NULL;
            if (LamportQueuePop(&queue_, &p)) {
                uint64_t id = (uint64_t)(uintptr_t)p;
                if (id != expected) {
                    pop_errors.fetch_add(1, std::memory_order_relaxed);
                }
                expected = id + 1;
                pop_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    start.store(true);
    producer.join();
    consumer.join();

    EXPECT_EQ(push_errors.load(), 0U);
    EXPECT_EQ(pop_errors.load(), 0U);
    EXPECT_EQ(push_count.load(), kNItems);
    EXPECT_EQ(pop_count.load(), kNItems);
    /* Queue should be empty */
    EXPECT_EQ(LamportQueueLeasedSize(&queue_), 0U);
}

TEST_F(LamportQueueTest, SpscConcurrentSustained) {
    Init(65536);
    constexpr int kDurationMs = 500;  /* run for 500ms */

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_pushes{0};
    std::atomic<uint64_t> total_pops{0};
    std::atomic<uint64_t> consistency_errors{0};

    /* Producer */
    std::thread producer([this, &start, &stop, &total_pushes]() {
        uint64_t seq = 0;
        while (!start.load()) { /* spin */ }
        while (!stop.load()) {
            if (LamportQueuePush(&queue_, AS_QUEUE_CLIENT_DATA((uintptr_t)(seq + 1)))) {
                total_pushes.fetch_add(1, std::memory_order_relaxed);
                seq++;
            }
        }
    });

    /* Consumer */
    std::thread consumer([this, &start, &stop, &total_pops,
                          &consistency_errors]() {
        uint64_t expected = 1;
        while (!start.load()) { /* spin */ }
        while (!stop.load()) {
            QueueClientData *p = NULL;
            if (LamportQueuePop(&queue_, &p)) {
                uint64_t id = (uint64_t)(uintptr_t)p;
                if (id != expected) {
                    consistency_errors.fetch_add(1, std::memory_order_relaxed);
                }
                expected = id + 1;
                total_pops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    start.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(kDurationMs));
    stop.store(true);

    producer.join();
    consumer.join();

    EXPECT_EQ(consistency_errors.load(), 0U);
    /* All pushed items should have been popped (or still in queue) */
    uint64_t in_queue = (uint64_t)LamportQueueLeasedSize(&queue_);
    EXPECT_EQ(total_pushes.load(), total_pops.load() + in_queue);
    /* At least some ops must have happened */
    EXPECT_GT(total_pushes.load(), 1000U);
    EXPECT_GT(total_pops.load(), 1000U);
    SUCCEED();
}
