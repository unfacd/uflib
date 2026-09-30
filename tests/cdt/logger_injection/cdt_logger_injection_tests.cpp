/**
 * @file cdt_logger_injection_tests.cpp
 * @brief The cdt modules' additive logger seam, held in place.
 *
 * Five lock-free structures in src/cdt/ accept a borrowed @c UfLogger pointer at
 * create time.  This suite exists to keep that contract honest, and it is
 * written to fail when the contract is broken rather than to record what the
 * code currently does:
 *
 *   - **Layout.**  Each struct carries deliberate cache-line padding, and the
 *     pad arithmetic is what keeps a hot cursor on its own line.  Adding the
 *     logger pointer had to come out of that padding, not out of the layout, so
 *     the offsets are asserted here: a future member that silently eats a pad
 *     breaks the build instead of quietly costing throughput.
 *
 *   - **NULL is the old behaviour.**  Every legacy constructor forwards NULL, so
 *     a caller that never heard of the logger must see exactly what it saw
 *     before.  A legacy constructor is also asserted to emit nothing at all.
 *
 *   - **The borrow reaches the sink, and is not ownership.**  After a module is
 *     destroyed the logger is still used: if the module had freed what it only
 *     borrowed, that is a use-after-free and the sanitizer says so.
 *
 *   - **Nothing is logged on a hot path.**  A concurrent push/pop run with the
 *     floor at TRACE must emit zero records.  That is the assertion that fails
 *     if a log site is ever added to a producer or consumer.
 *
 * mock_driver.c is the C face of the logger's private driver seam and is the
 * only file in this target that includes private logger headers; this
 * translation unit sees public types only.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "gtest/gtest.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
#include <uflib/standard_c_includes.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer.h>
#include <uflib/cdt/lockless_lru/cdt_lockless_lru.h>
#include <uflib/cdt/lockless_minheap/cdt_lockless_minheap.h>
#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack.h>
#include <uflib/cdt/chase_lev_stealing_queue/cdt_chase_lev_stealing_queue.h>
#include "cdt_lockless_ringbuffer_priv.h"
#include "cdt_lockless_lru_priv.h"
#include "cdt_lockless_minheap_priv.h"
#include "lockless_treiber_stack_priv.h"
#include "cdt_chase_lev_stealing_queue_priv.h"
}

#include "mock_driver.h"

/* ── Layout ─────────────────────────────────────────────────────────────── */
/*
 * Compile-time, so drift is a build failure rather than a red test.  The
 * invariant in each case is the one the module's own comment claims: the hot
 * cursor stays on the line it was put on, and the logger pointer sits in the
 * cold group without displacing anything.
 */

static_assert(sizeof(struct LocklessRingBuffer) == 192,
              "ring buffer grew: the logger pointer must come out of _pad_meta");
static_assert(offsetof(struct LocklessRingBuffer, tail) == 64,
              "tail must stay on cache line 1");
static_assert(offsetof(struct LocklessRingBuffer, head) == 128,
              "head must stay on cache line 2");
static_assert(offsetof(struct LocklessRingBuffer, uf_logger) < 64,
              "uf_logger belongs in the cold group");

static_assert(sizeof(struct LocklessLru) == 72,
              "LRU grew: the logger pointer must come out of _pad");
static_assert(offsetof(struct LocklessLru, clock_hand) < 64,
              "clock_hand must stay in line 0");
static_assert(offsetof(struct LocklessLru, count) == 64,
              "count must stay isolated on its own cache line");
static_assert(offsetof(struct LocklessLru, uf_logger) < 64,
              "uf_logger belongs in the cold group");

static_assert(offsetof(struct LocklessMinHeap, approx_size) == 64,
              "approx_size must stay isolated on its own cache line");
static_assert(offsetof(struct LocklessMinHeap, uf_logger) < 64,
              "uf_logger belongs in the cold group");

static_assert(sizeof(struct LocklessTreiberStack) == 16,
              "treiber stack is a head plus a borrow and nothing else");

static_assert(offsetof(struct ChaseLevStealingQueue, top) == 0, "top is line 0");
static_assert(offsetof(struct ChaseLevStealingQueue, bottom) == 64, "bottom is line 1");
static_assert(offsetof(struct ChaseLevStealingQueue, array) == 128, "array is line 2");
static_assert(offsetof(struct ChaseLevStealingQueue, capacity) == 192, "capacity is line 3");
static_assert(sizeof(struct ChaseLevStealingQueue) == 280,
              "the deque grew past its padded lines, which is where it may grow");

namespace {

/*!
 * A logger bound to the recording double, with the floor at TRACE so that every
 * severity a module can emit is observable.  ASSERT_* cannot be used in a
 * helper that returns, so the caller checks for NULL.
 */
UfLogger *MakeRecordingLogger(void)
{
    UfLogger *logger_ptr = nullptr;
    if (MockDriverCreateLogger(UfLoggerProvideSaneDefaults(), &logger_ptr) != UF_LOGGER_STATUS_OK) {
        return nullptr;
    }
    if (UfLoggerSetLevel(logger_ptr, UF_LOGGER_LEVEL_TRACE) != UF_LOGGER_STATUS_OK) {
        UfLoggerDestroy(logger_ptr);
        return nullptr;
    }
    return logger_ptr;
}

}  // namespace

/* ── NULL is the old behaviour ──────────────────────────────────────────── */

TEST(CdtLoggerInjectionNullLogger, LegacyConstructorsEmitNothing)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    /* The legacy constructors take no logger, so they must not merely pass NULL
       internally — they must produce no record on a logger that exists and is
       listening at the lowest possible floor. */
    LocklessRingBuffer *ring_ptr =
        LocklessRingBufferCreate(16, sizeof(int), LOCKLESS_RINGBUF_MODE_MPSC);
    ASSERT_NE(ring_ptr, nullptr);
    LocklessRingBufferDestroy(ring_ptr);

    LocklessMinHeap *pq_ptr = LocklessMinHeapCreate();
    ASSERT_NE(pq_ptr, nullptr);
    LocklessMinHeapDestroy(pq_ptr);

    ChaseLevStealingQueue *deque_ptr = ChaseLevStealingQueueCreate(0);
    ASSERT_NE(deque_ptr, nullptr);
    ChaseLevStealingQueueDestroy(deque_ptr);

    LocklessTreiberStack *stack_ptr = lockless_treiber_stack_create();
    ASSERT_NE(stack_ptr, nullptr);
    lockless_treiber_stack_destroy(stack_ptr);

    LocklessLru *lru_ptr = LocklessLruCreate(nullptr);
    ASSERT_NE(lru_ptr, nullptr);
    LocklessLruDestroy(lru_ptr);

    EXPECT_EQ(MockDriverWriteCount(), 0)
        << "a legacy constructor reached a logger it was never given";

    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionNullLogger, RingBufferWithNullLoggerBehavesAsBefore)
{
    LocklessRingBuffer *ring_ptr =
        LocklessRingBufferCreateWithLogger(1024, sizeof(uint64_t),
                                           LOCKLESS_RINGBUF_MODE_MPSC, nullptr);
    ASSERT_NE(ring_ptr, nullptr);

    EXPECT_EQ(LocklessRingBufferCapacity(ring_ptr), 1024u);
    EXPECT_TRUE(LocklessRingBufferEmpty(ring_ptr));

    for (uint64_t i = 0; i < 1000u; ++i) {
        ASSERT_TRUE(LocklessRingBufferTryPush(ring_ptr, &i));
    }
    EXPECT_EQ(LocklessRingBufferSize(ring_ptr), 1000u);

    for (uint64_t i = 0; i < 1000u; ++i) {
        uint64_t out = 0u;
        ASSERT_TRUE(LocklessRingBufferTryPop(ring_ptr, &out));
        EXPECT_EQ(out, i);
    }
    EXPECT_TRUE(LocklessRingBufferEmpty(ring_ptr));
    LocklessRingBufferDestroy(ring_ptr);
}

TEST(CdtLoggerInjectionNullLogger, LruWithNullLoggerBehavesAsBefore)
{
    LocklessLruConfig cfg = {};
    cfg.capacity_hint = 64;
    LocklessLru *lru_ptr = LocklessLruCreateWithLogger(&cfg, nullptr);
    ASSERT_NE(lru_ptr, nullptr);

    /* The LRU stores caller-owned pointers; a non-NULL one is enough here. */
    int payload = 42;
    LocklessLruSetResult set = LocklessLruSetEx(lru_ptr, 7u, (LruClientData *)&payload);
    EXPECT_EQ(set.status, LOCKLESS_LRU_SET_INSERTED);
    EXPECT_EQ(set.displaced, nullptr);

    EXPECT_EQ(LocklessLruGet(lru_ptr, 7u), (LruClientData *)&payload);

    LocklessLruDestroy(lru_ptr);
}

TEST(CdtLoggerInjectionNullLogger, MinHeapWithNullLoggerBehavesAsBefore)
{
    LocklessMinHeap *pq_ptr = LocklessMinHeapCreateWithLogger(nullptr);
    ASSERT_NE(pq_ptr, nullptr);

    int a = 1, b = 2;
    ASSERT_EQ(LocklessMinHeapInsertI64(pq_ptr, 20, &b), LOCKLESS_MINHEAP_OK);
    ASSERT_EQ(LocklessMinHeapInsertI64(pq_ptr, 10, &a), LOCKLESS_MINHEAP_OK);

    int64_t key = 0;
    void *value_ptr = nullptr;
    ASSERT_EQ(LocklessMinHeapDelminI64(pq_ptr, &key, &value_ptr), 1);
    EXPECT_EQ(key, 10);
    EXPECT_EQ(value_ptr, &a);

    LocklessMinHeapDestroy(pq_ptr);
}

/* ── The borrow reaches the sink ────────────────────────────────────────── */

TEST(CdtLoggerInjectionReporting, RingBufferReportsCreateAndRejections)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    LocklessRingBuffer *ring_ptr =
        LocklessRingBufferCreateWithLogger(64, sizeof(int), LOCKLESS_RINGBUF_MODE_MPSC, log_ptr);
    ASSERT_NE(ring_ptr, nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1)
        << "creation was not reported";

    /* Three rejections that used to be the same silent NULL.  Each must be
       reported, and reported as an error rather than swallowed. */
    MockDriverReset();
    EXPECT_EQ(LocklessRingBufferCreateWithLogger(0, sizeof(int),
                                                 LOCKLESS_RINGBUF_MODE_MPSC, log_ptr),
              nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_ERROR), 1);

    EXPECT_EQ(LocklessRingBufferCreateWithLogger(16, 0,
                                                 LOCKLESS_RINGBUF_MODE_MPSC, log_ptr),
              nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_ERROR), 2);

    EXPECT_EQ(LocklessRingBufferCreateWithLogger(16, sizeof(int),
                                                 (LocklessRingBufferMode)99, log_ptr),
              nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_ERROR), 3);

    LocklessRingBufferDestroy(ring_ptr);
    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionReporting, RingBufferReportsUnconsumedElementsAtTeardown)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    LocklessRingBuffer *ring_ptr =
        LocklessRingBufferCreateWithLogger(64, sizeof(int), LOCKLESS_RINGBUF_MODE_MPSC, log_ptr);
    ASSERT_NE(ring_ptr, nullptr);

    /* Push three and consume none: teardown must notice, and must get the
       arithmetic right rather than reporting the raw cursors. */
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(LocklessRingBufferTryPush(ring_ptr, &i));
    }

    MockDriverReset();
    LocklessRingBufferDestroy(ring_ptr);

    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_WARN), 1)
        << "unconsumed elements at teardown went unreported";

    const MockDriverRecord *record_ptr = MockDriverLastRecord();
    ASSERT_NE(record_ptr, nullptr);
    EXPECT_NE(std::strstr(record_ptr->message, "3 unconsumed"), nullptr)
        << "occupancy arithmetic is wrong; message was: " << record_ptr->message;

    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionReporting, RingBufferReportsEmptyTeardownAtDebug)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    LocklessRingBuffer *ring_ptr =
        LocklessRingBufferCreateWithLogger(64, sizeof(int), LOCKLESS_RINGBUF_MODE_MPSC, log_ptr);
    ASSERT_NE(ring_ptr, nullptr);

    int value = 1;
    ASSERT_TRUE(LocklessRingBufferTryPush(ring_ptr, &value));
    int out = 0;
    ASSERT_TRUE(LocklessRingBufferTryPop(ring_ptr, &out));

    MockDriverReset();
    LocklessRingBufferDestroy(ring_ptr);

    /* Drained, so this is not an anomaly and must not be raised as one. */
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_WARN), 0);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionReporting, MinHeapForwardsTheBorrowToItsRetireStack)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    MockDriverReset();
    LocklessMinHeap *pq_ptr = LocklessMinHeapCreateWithLogger(log_ptr);
    ASSERT_NE(pq_ptr, nullptr);

    /* Two halves, one logger: the heap's own creation record plus the retire
       stack's, which the heap created internally and had to forward to.  If the
       chain is ever dropped, this is one instead of two. */
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 2)
        << "the retire stack did not receive the forwarded borrow";

    MockDriverReset();
    LocklessMinHeapDestroy(pq_ptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 2)
        << "the retire stack's release went unreported";

    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionReporting, TreiberStackReportsBothEnds)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    MockDriverReset();
    LocklessTreiberStack *stack_ptr = lockless_treiber_stack_create_with_logger(log_ptr);
    ASSERT_NE(stack_ptr, nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    MockDriverReset();
    lockless_treiber_stack_destroy(stack_ptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionReporting, ChaseLevReportsBothEnds)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    MockDriverReset();
    ChaseLevStealingQueue *deque_ptr = ChaseLevStealingQueueCreateWithLogger(0, log_ptr);
    ASSERT_NE(deque_ptr, nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    MockDriverReset();
    ChaseLevStealingQueueDestroy(deque_ptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionReporting, LruReportsCreateAndRejections)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    LocklessLruConfig cfg = {};
    cfg.capacity_hint = 128;

    MockDriverReset();
    LocklessLru *lru_ptr = LocklessLruCreateWithLogger(&cfg, log_ptr);
    ASSERT_NE(lru_ptr, nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    MockDriverReset();
    /* A capacity hint whose headroom multiplication overflows size_t.  This is a
       rejection, not a silent NULL, and it is the only one reachable without
       forcing an allocation failure. */
    LocklessLruConfig huge = {};
    huge.capacity_hint = SIZE_MAX;
    EXPECT_EQ(LocklessLruCreateWithLogger(&huge, log_ptr), nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_ERROR), 1);

    LocklessLruDestroy(lru_ptr);
    UfLoggerDestroy(log_ptr);
}

/* ── The borrow is not ownership ────────────────────────────────────────── */

TEST(CdtLoggerInjectionBorrow, ModuleDoesNotDestroyWhatItBorrowed)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    LocklessRingBuffer *ring_ptr =
        LocklessRingBufferCreateWithLogger(16, sizeof(int), LOCKLESS_RINGBUF_MODE_SPSC, log_ptr);
    ASSERT_NE(ring_ptr, nullptr);
    LocklessRingBufferDestroy(ring_ptr);

    /* The module is gone.  Using the logger now is the point: a module that
       freed what it only borrowed makes this a use-after-free, which LSan
       reports rather than the expectation merely failing. */
    MockDriverReset();
    ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "borrow outlived the module"), UF_LOGGER_STATUS_OK);
    EXPECT_EQ(MockDriverWriteCount(), 1);

    /* And the same for the two structures that build a sub-structure: if either
       had taken ownership of the borrow, the other's use of it would show. */
    LocklessMinHeap *pq_ptr = LocklessMinHeapCreateWithLogger(log_ptr);
    ASSERT_NE(pq_ptr, nullptr);
    LocklessMinHeapDestroy(pq_ptr);

    MockDriverReset();
    ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "borrow outlived the heap and its retire stack"),
              UF_LOGGER_STATUS_OK);
    EXPECT_EQ(MockDriverWriteCount(), 1);

    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionBorrow, DestroyingTheLoggerLastIsTheContractedOrder)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    /* The documented order: every borrower released before the logger.  Doing it
       this way must be clean under LSan — a module still holding the borrow at
       logger destruction would be the other order, which is undefined and is
       therefore not asserted on here. */
    LocklessRingBuffer *ring_ptr =
        LocklessRingBufferCreateWithLogger(8, sizeof(int), LOCKLESS_RINGBUF_MODE_SPSC, log_ptr);
    ASSERT_NE(ring_ptr, nullptr);
    LocklessLru *lru_ptr = LocklessLruCreateWithLogger(nullptr, log_ptr);
    ASSERT_NE(lru_ptr, nullptr);
    LocklessTreiberStack *stack_ptr = lockless_treiber_stack_create_with_logger(log_ptr);
    ASSERT_NE(stack_ptr, nullptr);

    LocklessRingBufferDestroy(ring_ptr);
    LocklessLruDestroy(lru_ptr);
    lockless_treiber_stack_destroy(stack_ptr);
    UfLoggerDestroy(log_ptr);
}

/* ── The floor still governs ────────────────────────────────────────────── */

TEST(CdtLoggerInjectionFloor, FloorAboveEverySeveritySilencesWithoutBreaking)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    /* FATAL is above everything these modules emit, so the module's diagnostics
       are suppressed.  Suppressed must mean "carried on regardless", not
       "failed" and not "emitted anyway". */
    ASSERT_EQ(UfLoggerSetLevel(log_ptr, UF_LOGGER_LEVEL_FATAL), UF_LOGGER_STATUS_OK);

    MockDriverReset();
    LocklessRingBuffer *ring_ptr =
        LocklessRingBufferCreateWithLogger(32, sizeof(int), LOCKLESS_RINGBUF_MODE_MPSC, log_ptr);
    ASSERT_NE(ring_ptr, nullptr);

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(LocklessRingBufferTryPush(ring_ptr, &i));
    }
    EXPECT_EQ(LocklessRingBufferSize(ring_ptr), 5u);

    LocklessRingBufferDestroy(ring_ptr);
    EXPECT_EQ(MockDriverWriteCount(), 0) << "records were emitted below the floor";

    UfLoggerDestroy(log_ptr);
}

/* ── Nothing on a hot path ──────────────────────────────────────────────── */

TEST(CdtLoggerInjectionHotPath, ConcurrentPushPopEmitsNothing)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    LocklessRingBuffer *ring_ptr =
        LocklessRingBufferCreateWithLogger(1024, sizeof(uint64_t),
                                           LOCKLESS_RINGBUF_MODE_MPSC, log_ptr);
    ASSERT_NE(ring_ptr, nullptr);

    constexpr int kProducers = 4;
    constexpr uint64_t kPerProducer = 20000u;
    constexpr uint64_t kTotal = (uint64_t)kProducers * kPerProducer;

    /* Everything from here is hot path.  The floor is TRACE and the double is
       listening: if a log site existed on push or pop, it would fire here.  It
       would also serialise every operation through the double's mutex, which is
       the reason the rule exists. */
    MockDriverReset();

    std::atomic<uint64_t> consumed{0u};
    std::atomic<bool>     producers_done{false};

    std::thread consumer([&] {
        uint64_t value = 0u;
        while (consumed.load(std::memory_order_relaxed) < kTotal) {
            if (LocklessRingBufferTryPop(ring_ptr, &value)) {
                consumed.fetch_add(1u, std::memory_order_relaxed);
            }
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (uint64_t i = 0; i < kPerProducer; ++i) {
                uint64_t value = ((uint64_t)p << 32) | i;
                while (!LocklessRingBufferTryPush(ring_ptr, &value)) {
                    /* full — the consumer will drain it */
                }
            }
        });
    }
    for (std::thread &producer : producers) {
        producer.join();
    }
    producers_done.store(true, std::memory_order_relaxed);
    consumer.join();

    /* Exact accounting: every element produced was consumed exactly once. */
    EXPECT_EQ(consumed.load(), kTotal);
    EXPECT_TRUE(LocklessRingBufferEmpty(ring_ptr));

    EXPECT_EQ(MockDriverWriteCount(), 0)
        << "a record was emitted from the hot path; logging must stay off it";

    LocklessRingBufferDestroy(ring_ptr);
    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionHotPath, ConcurrentLruOperationsEmitNothing)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    LocklessLruConfig cfg = {};
    cfg.capacity_hint = 256;
    LocklessLru *lru_ptr = LocklessLruCreateWithLogger(&cfg, log_ptr);
    ASSERT_NE(lru_ptr, nullptr);

    MockDriverReset();

    constexpr int kThreads = 4;
    constexpr uint64_t kPerThread = 5000u;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (uint64_t i = 0; i < kPerThread; ++i) {
                uint64_t key = ((uint64_t)t << 32) | i;
                (void)LocklessLruSet(lru_ptr, key, (LruClientData *)(uintptr_t)(key | 1u));
                (void)LocklessLruGet(lru_ptr, key);
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }

    EXPECT_EQ(MockDriverWriteCount(), 0)
        << "a record was emitted from the hot path; logging must stay off it";

    LocklessLruDestroy(lru_ptr);
    UfLoggerDestroy(log_ptr);
}

/* ── Introspection reports the borrow ───────────────────────────────────── */

TEST(CdtLoggerInjectionDescribe, MinHeapReportsEnabledOnlyWhenALoggerWasInjected)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    /* Side by side: an answer hardcoded in either direction makes these two
       describe the same thing, which is what the pair is here to catch. */
    LocklessMinHeap *with_ptr = LocklessMinHeapCreateWithLogger(log_ptr);
    ASSERT_NE(with_ptr, nullptr);
    LocklessMinHeap *without_ptr = LocklessMinHeapCreate();
    ASSERT_NE(without_ptr, nullptr);

    BufferDescriptor with_bd;
    BufferDescriptorInit(&with_bd, 512);
    ASSERT_EQ(DescribeLocklessMinHeap(with_ptr, &with_bd), &with_bd);
    EXPECT_NE(std::strstr(with_bd.data, "\"logger\":\"enabled\""), nullptr)
        << "got: " << with_bd.data;
    BufferDescriptorRelease(&with_bd);

    BufferDescriptor without_bd;
    BufferDescriptorInit(&without_bd, 512);
    ASSERT_EQ(DescribeLocklessMinHeap(without_ptr, &without_bd), &without_bd);
    EXPECT_NE(std::strstr(without_bd.data, "\"logger\":\"none\""), nullptr)
        << "got: " << without_bd.data;
    BufferDescriptorRelease(&without_bd);

    LocklessMinHeapDestroy(with_ptr);
    LocklessMinHeapDestroy(without_ptr);
    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionDescribe, LruReportsEnabledOnlyWhenALoggerWasInjected)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    LocklessLruConfig cfg = {};
    cfg.capacity_hint = 32;

    LocklessLru *with_ptr = LocklessLruCreateWithLogger(&cfg, log_ptr);
    ASSERT_NE(with_ptr, nullptr);
    LocklessLru *without_ptr = LocklessLruCreateWithLogger(&cfg, nullptr);
    ASSERT_NE(without_ptr, nullptr);

    BufferDescriptor with_bd;
    BufferDescriptorInit(&with_bd, 1024);
    ASSERT_EQ(DescribeLocklessLru(with_ptr, &with_bd), &with_bd);
    EXPECT_NE(std::strstr(with_bd.data, "\"logger\":\"enabled\""), nullptr)
        << "got: " << with_bd.data;
    BufferDescriptorRelease(&with_bd);

    BufferDescriptor without_bd;
    BufferDescriptorInit(&without_bd, 1024);
    ASSERT_EQ(DescribeLocklessLru(without_ptr, &without_bd), &without_bd);
    EXPECT_NE(std::strstr(without_bd.data, "\"logger\":\"none\""), nullptr)
        << "got: " << without_bd.data;
    BufferDescriptorRelease(&without_bd);

    LocklessLruDestroy(with_ptr);
    LocklessLruDestroy(without_ptr);
    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionDescribe, MinHeapStaysEnabledAfterUse)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    /* The attribute reports a borrow, and a borrow does not lapse because the
       structure was used, drained or reclaimed.  If it were derived from
       anything but the stored pointer, one of these would show it. */
    LocklessMinHeap *pq_ptr = LocklessMinHeapCreateWithLogger(log_ptr);
    ASSERT_NE(pq_ptr, nullptr);

    int value = 1;
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(LocklessMinHeapInsertI64(pq_ptr, i, &value), LOCKLESS_MINHEAP_OK);
    }
    int64_t key = 0;
    void *out_ptr = nullptr;
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(LocklessMinHeapDelminI64(pq_ptr, &key, &out_ptr), 1);
    }
    LocklessMinHeapReclaim(pq_ptr);

    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 512);
    ASSERT_EQ(DescribeLocklessMinHeap(pq_ptr, &bd), &bd);
    EXPECT_NE(std::strstr(bd.data, "\"logger\":\"enabled\""), nullptr)
        << "got: " << bd.data;
    BufferDescriptorRelease(&bd);

    LocklessMinHeapDestroy(pq_ptr);
    UfLoggerDestroy(log_ptr);
}

TEST(CdtLoggerInjectionDescribe, NullHandleCarriesNoLoggerAttribute)
{
    /* A handle that does not exist has no borrow, so claiming one — in either
       direction — would be a fabricated answer.  The error object stands alone. */
    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 256);
    ASSERT_EQ(DescribeLocklessMinHeap(nullptr, &bd), &bd);
    EXPECT_NE(std::strstr(bd.data, "\"error\":\"null handle\""), nullptr);
    EXPECT_EQ(std::strstr(bd.data, "logger"), nullptr)
        << "got: " << bd.data;
    BufferDescriptorRelease(&bd);

    BufferDescriptor lru_bd;
    BufferDescriptorInit(&lru_bd, 256);
    ASSERT_EQ(DescribeLocklessLru(nullptr, &lru_bd), &lru_bd);
    EXPECT_NE(std::strstr(lru_bd.data, "\"error\":\"null handle\""), nullptr);
    EXPECT_EQ(std::strstr(lru_bd.data, "logger"), nullptr)
        << "got: " << lru_bd.data;
    BufferDescriptorRelease(&lru_bd);
}
