/**
 * @file cdt_mpsc_queue_tests.cpp
 * @brief Comprehensive unit tests for the intrusive lock-free MPSC queue
 *        (LocklessMpscQueue, Vyukov algorithm).
 *
 * Covers lifecycle, single-threaded FIFO semantics, push_front, traversal
 * macros, poll semantics, ABI layout lock, and MPSC concurrent safety
 * (exactly-once delivery, per-producer FIFO, mixed workload, high
 * contention, RETRY-window exercise).
 *
 * Copyright (C) 2015-2026 unfacd works
 */

#include "gtest/gtest.h"

#include <cstring>
#include <cstdint>
#include <cstddef>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>

extern "C" {
#include "uflib/cdt/cdt_mpsc_queue.h"
}

/* The MPSC_QUEUE_FOR_EACH macro expands to an unqualified
 * memory_order_acquire; in C++ that enumerator lives in std::. */
using std::memory_order_acquire;

/* ── Helpers ────────────────────────────────────────────────────────────── */

namespace {

/* Encode (producer id, sequence) into an opaque payload pointer. */
inline QueueContextData *
EncodePayload(uint32_t producer, uint64_t seq)
{
    return AS_QUEUE_CONTEXT_DATA((uintptr_t)(((uint64_t)producer << 48) | seq));
}

inline uint32_t DecodeProducer(QueueContextData *p) {
    return (uint32_t)(((uint64_t)(uintptr_t)p) >> 48);
}

inline uint64_t DecodeSeq(QueueContextData *p) {
    return ((uint64_t)(uintptr_t)p) & ((1ULL << 48) - 1);
}

} // namespace

class MpscQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        mpsc_queue_init(&queue_);
    }

    LocklessMpscQueue queue_;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * ABI layout lock — the packed layout is frozen (downstream embeds by value
 * and sizes pools from sizeof); any change here is a MAJOR-class break.
 * ═══════════════════════════════════════════════════════════════════════════ */

#if defined(__LP64__)
static_assert(sizeof(struct mpsc_queue_node) == 32,
              "ABI FREEZE VIOLATION: mpsc_queue_node layout changed");
static_assert(sizeof(LocklessMpscQueue) == 192,
              "ABI FREEZE VIOLATION: LocklessMpscQueue layout changed");
static_assert(offsetof(LocklessMpscQueue, head) == 0,
              "ABI FREEZE VIOLATION: head offset changed");
static_assert(offsetof(LocklessMpscQueue, tail) == 64,
              "ABI FREEZE VIOLATION: tail not on its own cache line");
static_assert(offsetof(LocklessMpscQueue, stub) == 128,
              "ABI FREEZE VIOLATION: stub not on its own cache line");
#endif

TEST_F(MpscQueueTest, AbiLayoutLockLp64) {
#if defined(__LP64__)
    EXPECT_EQ(sizeof(struct mpsc_queue_node), 32U);
    EXPECT_EQ(sizeof(LocklessMpscQueue), 192U);
#else
    GTEST_SKIP() << "ABI lock asserted for LP64 only";
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Init / lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(MpscQueueTest, InitValid) {
    EXPECT_EQ(atomic_load_explicit(&queue_.head, std::memory_order_relaxed),
              &queue_.stub);
    EXPECT_EQ(atomic_load_explicit(&queue_.tail, std::memory_order_relaxed),
              &queue_.stub);
    EXPECT_EQ(atomic_load_explicit(&queue_.stub.next, std::memory_order_relaxed),
              nullptr);
}

TEST_F(MpscQueueTest, InitNullSafe) {
    mpsc_queue_init(nullptr);  /* must not crash */
}

TEST_F(MpscQueueTest, ReInitAfterUse) {
    mpsc_queue_node node{};
    node.context_data = EncodePayload(0, 1);
    mpsc_queue_insert(&queue_, &node);

    mpsc_queue_init(&queue_);  /* discard state, back to empty */
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Empty-queue behaviour + NULL guards
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(MpscQueueTest, PopOnEmptyReturnsNull) {
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

TEST_F(MpscQueueTest, TailOnEmptyReturnsNull) {
    EXPECT_EQ(mpsc_queue_tail(&queue_), nullptr);
}

TEST_F(MpscQueueTest, PollOnEmptyReturnsEmpty) {
    mpsc_queue_node *node = nullptr;
    EXPECT_EQ(mpsc_queue_poll(&queue_, &node), MPSC_QUEUE_EMPTY);
}

TEST_F(MpscQueueTest, NullGuards) {
    mpsc_queue_node node{};
    mpsc_queue_node *out = nullptr;

    EXPECT_EQ(mpsc_queue_pop(nullptr), nullptr);
    EXPECT_EQ(mpsc_queue_tail(nullptr), nullptr);
    EXPECT_EQ(mpsc_queue_poll(nullptr, &out), MPSC_QUEUE_EMPTY);
    EXPECT_EQ(mpsc_queue_poll(&queue_, nullptr), MPSC_QUEUE_EMPTY);
    mpsc_queue_insert(nullptr, &node);       /* no crash */
    mpsc_queue_insert(&queue_, nullptr);     /* no crash */
    mpsc_queue_push_front(nullptr, &node);   /* no crash */
    mpsc_queue_push_front(&queue_, nullptr); /* no crash */

    /* Queue must still be functional and empty. */
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-thread FIFO semantics
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(MpscQueueTest, InsertPopSingle) {
    mpsc_queue_node node{};
    node.context_data = EncodePayload(7, 42);

    mpsc_queue_insert(&queue_, &node);
    mpsc_queue_node *popped = mpsc_queue_pop(&queue_);

    ASSERT_EQ(popped, &node);
    EXPECT_EQ(DecodeProducer(popped->context_data), 7U);
    EXPECT_EQ(DecodeSeq(popped->context_data), 42U);
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

TEST_F(MpscQueueTest, InsertPopFifoOrder) {
    constexpr int kCount = 64;
    mpsc_queue_node nodes[kCount] = {};

    for (int i = 0; i < kCount; i++) {
        nodes[i].context_data = EncodePayload(0, (uint64_t)i);
        mpsc_queue_insert(&queue_, &nodes[i]);
    }
    for (int i = 0; i < kCount; i++) {
        mpsc_queue_node *popped = mpsc_queue_pop(&queue_);
        ASSERT_NE(popped, nullptr) << "premature empty at " << i;
        EXPECT_EQ(DecodeSeq(popped->context_data), (uint64_t)i);
    }
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

TEST_F(MpscQueueTest, DrainToEmptyThenReuse) {
    mpsc_queue_node a{}, b{};
    a.context_data = EncodePayload(0, 1);
    b.context_data = EncodePayload(0, 2);

    mpsc_queue_insert(&queue_, &a);
    EXPECT_EQ(mpsc_queue_pop(&queue_), &a);   /* drains — stub re-primed */
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);

    mpsc_queue_insert(&queue_, &b);           /* queue must still work */
    EXPECT_EQ(mpsc_queue_pop(&queue_), &b);
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

TEST_F(MpscQueueTest, RepeatedDrainCycles) {
    mpsc_queue_node node{};
    for (int cycle = 0; cycle < 1000; cycle++) {
        node.context_data = EncodePayload(0, (uint64_t)cycle);
        mpsc_queue_insert(&queue_, &node);
        ASSERT_EQ(mpsc_queue_pop(&queue_), &node) << "cycle " << cycle;
        ASSERT_EQ(mpsc_queue_pop(&queue_), nullptr);
    }
}

TEST_F(MpscQueueTest, NodeReusableAfterPop) {
    mpsc_queue_node node{};
    node.context_data = EncodePayload(0, 1);

    mpsc_queue_insert(&queue_, &node);
    ASSERT_EQ(mpsc_queue_pop(&queue_), &node);

    node.context_data = EncodePayload(0, 2);  /* same storage, new payload */
    mpsc_queue_insert(&queue_, &node);
    mpsc_queue_node *popped = mpsc_queue_pop(&queue_);
    ASSERT_EQ(popped, &node);
    EXPECT_EQ(DecodeSeq(popped->context_data), 2U);
}

TEST_F(MpscQueueTest, PayloadAndFinaliserPassThrough) {
    int sentinel = 0;
    mpsc_queue_node node{};
    node.context_data = EncodePayload(3, 9);
    node.finaliser.callback = nullptr;
    node.finaliser.context_data = &sentinel;

    mpsc_queue_insert(&queue_, &node);
    mpsc_queue_node *popped = mpsc_queue_pop(&queue_);

    ASSERT_EQ(popped, &node);
    EXPECT_EQ(popped->finaliser.context_data, &sentinel);
    EXPECT_EQ(popped->finaliser.callback, nullptr);
    EXPECT_EQ(DecodeProducer(popped->context_data), 3U);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Poll semantics
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(MpscQueueTest, PollReturnsItem) {
    mpsc_queue_node node{};
    node.context_data = EncodePayload(0, 5);
    mpsc_queue_insert(&queue_, &node);

    mpsc_queue_node *out = nullptr;
    EXPECT_EQ(mpsc_queue_poll(&queue_, &out), MPSC_QUEUE_ITEM);
    EXPECT_EQ(out, &node);
    EXPECT_EQ(mpsc_queue_poll(&queue_, &out), MPSC_QUEUE_EMPTY);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * push_front (consumer-only front insert)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(MpscQueueTest, PushFrontOnEmpty) {
    mpsc_queue_node node{};
    node.context_data = EncodePayload(0, 1);

    mpsc_queue_push_front(&queue_, &node);
    EXPECT_EQ(mpsc_queue_pop(&queue_), &node);
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

TEST_F(MpscQueueTest, PushFrontRequeuesPoppedNode) {
    mpsc_queue_node a{}, b{};
    a.context_data = EncodePayload(0, 1);
    b.context_data = EncodePayload(0, 2);

    mpsc_queue_insert(&queue_, &a);
    mpsc_queue_insert(&queue_, &b);

    mpsc_queue_node *popped = mpsc_queue_pop(&queue_);
    ASSERT_EQ(popped, &a);
    mpsc_queue_push_front(&queue_, popped);   /* undo the pop */

    EXPECT_EQ(mpsc_queue_pop(&queue_), &a);   /* order preserved */
    EXPECT_EQ(mpsc_queue_pop(&queue_), &b);
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

TEST_F(MpscQueueTest, PushFrontIsLifoAtFront) {
    mpsc_queue_node x{}, y{};
    x.context_data = EncodePayload(0, 1);
    y.context_data = EncodePayload(0, 2);

    mpsc_queue_push_front(&queue_, &x);
    mpsc_queue_push_front(&queue_, &y);       /* y now in front of x */

    EXPECT_EQ(mpsc_queue_pop(&queue_), &y);
    EXPECT_EQ(mpsc_queue_pop(&queue_), &x);
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Peek + traversal macros
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_F(MpscQueueTest, TailPeeksWithoutRemoving) {
    mpsc_queue_node a{}, b{};
    a.context_data = EncodePayload(0, 1);
    b.context_data = EncodePayload(0, 2);

    mpsc_queue_insert(&queue_, &a);
    mpsc_queue_insert(&queue_, &b);

    EXPECT_EQ(mpsc_queue_tail(&queue_), &a);  /* peek */
    EXPECT_EQ(mpsc_queue_tail(&queue_), &a);  /* idempotent */
    EXPECT_EQ(mpsc_queue_pop(&queue_), &a);   /* still dequeues first */
    EXPECT_EQ(mpsc_queue_pop(&queue_), &b);
}

TEST_F(MpscQueueTest, ForEachTraversesWithoutDequeue) {
    constexpr int kCount = 8;
    mpsc_queue_node nodes[kCount] = {};
    for (int i = 0; i < kCount; i++) {
        nodes[i].context_data = EncodePayload(0, (uint64_t)i);
        mpsc_queue_insert(&queue_, &nodes[i]);
    }

    int visited = 0;
    mpsc_queue_node *node;
    MPSC_QUEUE_FOR_EACH(node, &queue_) {
        EXPECT_EQ(DecodeSeq(node->context_data), (uint64_t)visited);
        visited++;
    }
    EXPECT_EQ(visited, kCount);

    /* Traversal must not have consumed anything. */
    for (int i = 0; i < kCount; i++) {
        ASSERT_EQ(mpsc_queue_pop(&queue_), &nodes[i]);
    }
}

TEST_F(MpscQueueTest, ForEachPopDrains) {
    constexpr int kCount = 8;
    mpsc_queue_node nodes[kCount] = {};
    for (int i = 0; i < kCount; i++) {
        nodes[i].context_data = EncodePayload(0, (uint64_t)i);
        mpsc_queue_insert(&queue_, &nodes[i]);
    }

    int drained = 0;
    mpsc_queue_node *node;
    MPSC_QUEUE_FOR_EACH_POP(node, &queue_) {
        EXPECT_EQ(DecodeSeq(node->context_data), (uint64_t)drained);
        drained++;
    }
    EXPECT_EQ(drained, kCount);
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrent scenarios (MPSC: N producers, one consumer)
 * ═══════════════════════════════════════════════════════════════════════════ */

namespace {

/* Pop until `total` nodes received or `deadline_s` elapses; returns count. */
uint64_t
ConsumeAll(LocklessMpscQueue *queue, uint64_t total,
           std::vector<std::atomic<uint32_t>> *seen,     /* per-global-id counter */
           std::vector<uint64_t> *next_seq_per_producer, /* per-producer FIFO check */
           uint64_t *fifo_violations, int deadline_s = 60)
{
    uint64_t received = 0;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(deadline_s);

    while (received < total &&
           std::chrono::steady_clock::now() < deadline) {
        mpsc_queue_node *node = mpsc_queue_pop(queue);
        if (node == nullptr) {
            std::this_thread::yield();
            continue;
        }
        uint32_t producer = DecodeProducer(node->context_data);
        uint64_t seq      = DecodeSeq(node->context_data);

        if (next_seq_per_producer) {
            if (seq != (*next_seq_per_producer)[producer]) (*fifo_violations)++;
            (*next_seq_per_producer)[producer] = seq + 1;
        }
        received++;
        if (seen) (*seen)[(size_t)producer].fetch_add(1, std::memory_order_relaxed);
    }
    return received;
}

} // namespace

TEST_F(MpscQueueTest, ConcurrentProducersExactlyOnce) {
    constexpr uint32_t kProducers = 8;
    constexpr uint64_t kPerProducer = 5000;

    std::vector<std::unique_ptr<mpsc_queue_node[]>> pools;
    for (uint32_t p = 0; p < kProducers; p++)
        pools.emplace_back(new mpsc_queue_node[kPerProducer]());

    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < kProducers; p++) {
        producers.emplace_back([this, p, &pools]() {
            for (uint64_t i = 0; i < kPerProducer; i++) {
                pools[p][i].context_data = EncodePayload(p, i);
                mpsc_queue_insert(&queue_, &pools[p][i]);
            }
        });
    }

    std::vector<std::atomic<uint32_t>> per_producer_count(kProducers);
    uint64_t received = ConsumeAll(&queue_, kProducers * kPerProducer,
                                   &per_producer_count, nullptr, nullptr);

    for (auto &t : producers) t.join();

    EXPECT_EQ(received, kProducers * kPerProducer) << "lost nodes";
    for (uint32_t p = 0; p < kProducers; p++) {
        EXPECT_EQ(per_producer_count[p].load(), kPerProducer)
            << "producer " << p << " delivery count wrong (dup or loss)";
    }
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

TEST_F(MpscQueueTest, ConcurrentPerProducerFifo) {
    constexpr uint32_t kProducers = 6;
    constexpr uint64_t kPerProducer = 5000;

    std::vector<std::unique_ptr<mpsc_queue_node[]>> pools;
    for (uint32_t p = 0; p < kProducers; p++)
        pools.emplace_back(new mpsc_queue_node[kPerProducer]());

    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < kProducers; p++) {
        producers.emplace_back([this, p, &pools]() {
            for (uint64_t i = 0; i < kPerProducer; i++) {
                pools[p][i].context_data = EncodePayload(p, i);
                mpsc_queue_insert(&queue_, &pools[p][i]);
            }
        });
    }

    std::vector<uint64_t> next_seq(kProducers, 0);
    uint64_t fifo_violations = 0;
    uint64_t received = ConsumeAll(&queue_, kProducers * kPerProducer,
                                   nullptr, &next_seq, &fifo_violations);

    for (auto &t : producers) t.join();

    EXPECT_EQ(received, kProducers * kPerProducer);
    EXPECT_EQ(fifo_violations, 0U) << "per-producer FIFO order violated";
}

TEST_F(MpscQueueTest, ConcurrentMixedWorkloadWithPushFront) {
    constexpr uint32_t kProducers = 4;
    constexpr uint64_t kPerProducer = 4000;

    std::vector<std::unique_ptr<mpsc_queue_node[]>> pools;
    for (uint32_t p = 0; p < kProducers; p++)
        pools.emplace_back(new mpsc_queue_node[kPerProducer]());

    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < kProducers; p++) {
        producers.emplace_back([this, p, &pools]() {
            for (uint64_t i = 0; i < kPerProducer; i++) {
                pools[p][i].context_data = EncodePayload(p, i);
                mpsc_queue_insert(&queue_, &pools[p][i]);
                if ((i & 0x3FF) == 0) std::this_thread::yield();
            }
        });
    }

    /* Consumer: every 16th node is requeued at the front exactly once
     * (simulating "cannot process yet"), then consumed on re-encounter. */
    uint64_t consumed = 0, requeued = 0;
    const uint64_t total = kProducers * kPerProducer;
    std::vector<std::vector<uint8_t>> seen(kProducers,
                                           std::vector<uint8_t>(kPerProducer, 0));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

    while (consumed < total &&
           std::chrono::steady_clock::now() < deadline) {
        mpsc_queue_node *node = mpsc_queue_pop(&queue_);
        if (node == nullptr) { std::this_thread::yield(); continue; }

        uint32_t p = DecodeProducer(node->context_data);
        uint64_t s = DecodeSeq(node->context_data);
        ASSERT_LT(p, kProducers);
        ASSERT_LT(s, kPerProducer);

        if ((s & 0xF) == 0 && seen[p][s] == 0) {
            seen[p][s] = 1;                       /* first encounter: requeue */
            mpsc_queue_push_front(&queue_, node);
            requeued++;
            continue;
        }
        ASSERT_LT(seen[p][s], 2) << "node consumed twice";
        seen[p][s] = 2;
        consumed++;
    }

    for (auto &t : producers) t.join();

    EXPECT_EQ(consumed, total) << "lost nodes (requeued=" << requeued << ")";
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

TEST_F(MpscQueueTest, HighContentionBurst) {
    const uint32_t kProducers = std::max(12U, std::thread::hardware_concurrency());
    constexpr uint64_t kPerProducer = 2000;

    std::vector<std::unique_ptr<mpsc_queue_node[]>> pools;
    for (uint32_t p = 0; p < kProducers; p++)
        pools.emplace_back(new mpsc_queue_node[kPerProducer]());

    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < kProducers; p++) {
        producers.emplace_back([this, p, &pools, &go]() {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (uint64_t i = 0; i < kPerProducer; i++) {
                pools[p][i].context_data = EncodePayload(p, i);
                mpsc_queue_insert(&queue_, &pools[p][i]);
            }
        });
    }
    go.store(true, std::memory_order_release);   /* burst start */

    std::vector<uint64_t> next_seq(kProducers, 0);
    uint64_t fifo_violations = 0;
    uint64_t received = ConsumeAll(&queue_, (uint64_t)kProducers * kPerProducer,
                                   nullptr, &next_seq, &fifo_violations);

    for (auto &t : producers) t.join();

    EXPECT_EQ(received, (uint64_t)kProducers * kPerProducer);
    EXPECT_EQ(fifo_violations, 0U);
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}

TEST_F(MpscQueueTest, RetryWindowExercisedByPoll) {
    constexpr uint32_t kProducers = 4;
    constexpr uint64_t kPerProducer = 3000;

    std::vector<std::unique_ptr<mpsc_queue_node[]>> pools;
    for (uint32_t p = 0; p < kProducers; p++)
        pools.emplace_back(new mpsc_queue_node[kPerProducer]());

    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < kProducers; p++) {
        producers.emplace_back([this, p, &pools]() {
            for (uint64_t i = 0; i < kPerProducer; i++) {
                pools[p][i].context_data = EncodePayload(p, i);
                mpsc_queue_insert(&queue_, &pools[p][i]);
                std::this_thread::yield();   /* widen the two-step window */
            }
        });
    }

    /* Consume via raw poll() so RETRY is observable rather than hidden. */
    uint64_t received = 0, retries = 0, empties = 0;
    const uint64_t total = kProducers * kPerProducer;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

    while (received < total &&
           std::chrono::steady_clock::now() < deadline) {
        mpsc_queue_node *node = nullptr;
        switch (mpsc_queue_poll(&queue_, &node)) {
            case MPSC_QUEUE_ITEM:  received++; break;
            case MPSC_QUEUE_RETRY: retries++;  std::this_thread::yield(); break;
            case MPSC_QUEUE_EMPTY: empties++;  std::this_thread::yield(); break;
        }
    }

    for (auto &t : producers) t.join();

    EXPECT_EQ(received, total)
        << "poll-driven consumption incomplete (retries=" << retries
        << " empties=" << empties << ")";
    EXPECT_EQ(mpsc_queue_pop(&queue_), nullptr);
}
