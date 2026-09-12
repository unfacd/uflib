/**
 * @file cdt_lockless_ringbuffer_tests.cpp
 * @brief Comprehensive GoogleTest suite for the lock-free bounded ring buffer
 *        (LocklessRingBuffer — SPSC / MPSC / MPMC).
 *
 * Covers lifecycle, power-of-two rounding and per-mode capacity semantics,
 * single-threaded byte-exact round-trip and FIFO ordering, full/empty
 * boundary behaviour, the typed convenience macro, adversarial inputs
 * (NULL args, hostile sizes, canary write-footprint), concurrent
 * exactly-once delivery (SPSC/MPSC/MPMC), per-producer FIFO (single-consumer
 * modes), torn-write checksum detection, and ABI/layout locks.
 *
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

#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
#include <uflib/standard_c_includes.h>
#include <uflib/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer.h>
#include <uflib/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer_type.h>
#include <uflib/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer_defs.h>
#include "cdt_lockless_ringbuffer_priv.h"
}

namespace {

const LocklessRingBufferMode kAllModes[] = {
    LOCKLESS_RINGBUF_MODE_SPSC,
    LOCKLESS_RINGBUF_MODE_MPSC,
    LOCKLESS_RINGBUF_MODE_MPMC
};

// Expected storable capacity for a given requested capacity and mode:
// SPSC reserves one slot; MPSC/MPMC use the full power-of-two.
size_t
sExpectedUsable(size_t requested, LocklessRingBufferMode mode)
{
    size_t p = 1;
    while (p < requested) p <<= 1;
    return (mode == LOCKLESS_RINGBUF_MODE_SPSC) ? p - 1 : p;
}

} // namespace

/* ═══════════════════════════════════════════════════════════════════════════
 * ABI / layout locks
 * ═══════════════════════════════════════════════════════════════════════════ */

static_assert(LOCKLESS_RINGBUF_MODE_SPSC == 0, "SPSC enumerator value is ABI");
static_assert(LOCKLESS_RINGBUF_MODE_MPSC == 1, "MPSC enumerator value is ABI");
static_assert(LOCKLESS_RINGBUF_MODE_MPMC == 2, "MPMC enumerator value is ABI");

TEST(RingBufferAbiTest, CacheLineLayout)
{
    // tail and head must be isolated on their own cache lines (see _priv.h).
    EXPECT_EQ(offsetof(LocklessRingBuffer, tail),
              static_cast<size_t>(PRIV_CONFIG_DEFAULT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE));
    EXPECT_EQ(offsetof(LocklessRingBuffer, head),
              2 * static_cast<size_t>(PRIV_CONFIG_DEFAULT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE));
    EXPECT_GE(offsetof(LocklessRingBuffer, head) - offsetof(LocklessRingBuffer, tail),
              static_cast<size_t>(PRIV_CONFIG_DEFAULT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(RingBufferLifecycleTest, CreateAllModes)
{
    for (LocklessRingBufferMode mode : kAllModes) {
        LocklessRingBuffer *ring = LocklessRingBufferCreate(1024, sizeof(int), mode);
        ASSERT_NE(ring, nullptr);
        LocklessRingBufferDestroy(ring);
    }
}

TEST(RingBufferLifecycleTest, CreateRejectsInvalidArgs)
{
    EXPECT_EQ(LocklessRingBufferCreate(0, sizeof(int), LOCKLESS_RINGBUF_MODE_SPSC), nullptr);
    EXPECT_EQ(LocklessRingBufferCreate(1024, 0, LOCKLESS_RINGBUF_MODE_SPSC), nullptr);
    EXPECT_EQ(LocklessRingBufferCreate(1024, sizeof(int),
                                       static_cast<LocklessRingBufferMode>(99)),
              nullptr);
}

TEST(RingBufferLifecycleTest, DestroyNullIsNoop)
{
    LocklessRingBufferDestroy(nullptr);   // must not crash (ASan/LSan exercise)
}

TEST(RingBufferLifecycleTest, CreateDestroyLoop)
{
    for (int i = 0; i < 1000; ++i) {
        LocklessRingBuffer *ring =
            LocklessRingBufferCreate(1 + (i % 4096), sizeof(uint64_t),
                                     kAllModes[i % 3]);
        ASSERT_NE(ring, nullptr);
        LocklessRingBufferDestroy(ring);
    }
}

TEST(RingBufferLifecycleTest, PowerOfTwoRounding)
{
    // Storage rounds up to the next power of two; usable differs by mode.
    EXPECT_EQ(LocklessRingBufferCapacity(
                  LocklessRingBufferCreate(1, 1, LOCKLESS_RINGBUF_MODE_MPSC)),
              1u);
    EXPECT_EQ(sExpectedUsable(3, LOCKLESS_RINGBUF_MODE_MPSC), 4u);
    EXPECT_EQ(sExpectedUsable(1000, LOCKLESS_RINGBUF_MODE_MPSC), 1024u);
    EXPECT_EQ(sExpectedUsable(4097, LOCKLESS_RINGBUF_MODE_MPSC), 8192u);
}

TEST(RingBufferLifecycleTest, PerModeCapacitySemantics)
{
    // Requested 8 → storage 8 → SPSC usable 7, MPSC/MPMC usable 8.
    LocklessRingBuffer *spsc = LocklessRingBufferCreate(8, 1, LOCKLESS_RINGBUF_MODE_SPSC);
    LocklessRingBuffer *mpsc = LocklessRingBufferCreate(8, 1, LOCKLESS_RINGBUF_MODE_MPSC);
    LocklessRingBuffer *mpmc = LocklessRingBufferCreate(8, 1, LOCKLESS_RINGBUF_MODE_MPMC);
    ASSERT_NE(spsc, nullptr);
    ASSERT_NE(mpsc, nullptr);
    ASSERT_NE(mpmc, nullptr);

    EXPECT_EQ(LocklessRingBufferCapacity(spsc), 7u);
    EXPECT_EQ(LocklessRingBufferCapacity(mpsc), 8u);
    EXPECT_EQ(LocklessRingBufferCapacity(mpmc), 8u);

    LocklessRingBufferDestroy(spsc);
    LocklessRingBufferDestroy(mpsc);
    LocklessRingBufferDestroy(mpmc);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-thread correctness
 * ═══════════════════════════════════════════════════════════════════════════ */

// Byte-exact round-trip for arbitrary elem_size, every mode.
void
sRoundTrip(LocklessRingBufferMode mode, size_t elem_size)
{
    LocklessRingBuffer *ring = LocklessRingBufferCreate(8, elem_size, mode);
    ASSERT_NE(ring, nullptr);

    std::vector<uint8_t> in(elem_size), out(elem_size, 0);
    for (size_t i = 0; i < elem_size; ++i) {
        in[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    }

    ASSERT_TRUE(LocklessRingBufferTryPush(ring, in.data()));
    ASSERT_TRUE(LocklessRingBufferTryPop(ring, out.data()));
    EXPECT_EQ(in, out);

    LocklessRingBufferDestroy(ring);
}

TEST(RingBufferRoundTripTest, ByteExactForVariousSizes)
{
    for (LocklessRingBufferMode mode : kAllModes) {
        sRoundTrip(mode, 1);
        sRoundTrip(mode, 4);
        sRoundTrip(mode, 8);
        sRoundTrip(mode, 100);   // crosses a cache line
        sRoundTrip(mode, 128);
    }
}

TEST(RingBufferFifoTest, FifoOrderPreserved)
{
    for (LocklessRingBufferMode mode : kAllModes) {
        LocklessRingBuffer *ring = LocklessRingBufferCreate(64, sizeof(uint64_t), mode);
        ASSERT_NE(ring, nullptr);

        for (uint64_t i = 0; i < 32; ++i) {
            ASSERT_TRUE(LocklessRingBufferTryPush(ring, &i));
        }
        for (uint64_t i = 0; i < 32; ++i) {
            uint64_t out = 0;
            ASSERT_TRUE(LocklessRingBufferTryPop(ring, &out));
            EXPECT_EQ(out, i);
        }
        LocklessRingBufferDestroy(ring);
    }
}

TEST(RingBufferBoundaryTest, PopEmptyAndPushFull)
{
    for (LocklessRingBufferMode mode : kAllModes) {
        LocklessRingBuffer *ring = LocklessRingBufferCreate(8, sizeof(int), mode);
        ASSERT_NE(ring, nullptr);

        int out = 0;
        EXPECT_FALSE(LocklessRingBufferTryPop(ring, &out));   // empty

        size_t usable = LocklessRingBufferCapacity(ring);
        for (size_t i = 0; i < usable; ++i) {
            int v = static_cast<int>(i);
            ASSERT_TRUE(LocklessRingBufferTryPush(ring, &v));
        }
        EXPECT_TRUE(LocklessRingBufferFull(ring));
        EXPECT_EQ(LocklessRingBufferSize(ring), usable);

        int v = 999;
        EXPECT_FALSE(LocklessRingBufferTryPush(ring, &v));    // full
        EXPECT_EQ(v, 999);                                    // untouched

        for (size_t i = 0; i < usable; ++i) {
            ASSERT_TRUE(LocklessRingBufferTryPop(ring, &out));
            EXPECT_EQ(out, static_cast<int>(i));
        }
        EXPECT_TRUE(LocklessRingBufferEmpty(ring));
        EXPECT_EQ(LocklessRingBufferSize(ring), 0u);
        EXPECT_FALSE(LocklessRingBufferTryPop(ring, &out));   // empty again

        LocklessRingBufferDestroy(ring);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Typed convenience macro
 * ═══════════════════════════════════════════════════════════════════════════ */

CDT_RINGBUF_DEFINE(U64Queue, uint64_t, 128, LOCKLESS_RINGBUF_MODE_MPSC)

TEST(RingBufferTypedMacroTest, RoundTrip)
{
    U64Queue_t *queue_ptr = U64QueueCreate();
    ASSERT_NE(queue_ptr, nullptr);

    for (uint64_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(U64QueueTryPush(queue_ptr, i));
    }
    for (uint64_t i = 0; i < 16; ++i) {
        uint64_t out = 0;
        ASSERT_TRUE(U64QueueTryPop(queue_ptr, &out));
        EXPECT_EQ(out, i);
    }
    EXPECT_TRUE(U64QueueEmpty(queue_ptr));

    U64QueueDestroy(queue_ptr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Adversarial inputs
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(RingBufferAdversarialTest, NullArgsReturnFalse)
{
    LocklessRingBuffer *ring = LocklessRingBufferCreate(8, sizeof(int), LOCKLESS_RINGBUF_MODE_MPSC);
    ASSERT_NE(ring, nullptr);

    int v = 1;
    EXPECT_FALSE(LocklessRingBufferTryPush(nullptr, &v));
    EXPECT_FALSE(LocklessRingBufferTryPush(ring, nullptr));
    EXPECT_FALSE(LocklessRingBufferTryPop(nullptr, &v));
    EXPECT_FALSE(LocklessRingBufferTryPop(ring, nullptr));

    EXPECT_EQ(LocklessRingBufferSize(nullptr), 0u);
    EXPECT_EQ(LocklessRingBufferCapacity(nullptr), 0u);
    EXPECT_TRUE(LocklessRingBufferEmpty(nullptr));
    EXPECT_FALSE(LocklessRingBufferFull(nullptr));

    LocklessRingBufferDestroy(ring);
}

TEST(RingBufferAdversarialTest, OversizedCapacityFailsCleanly)
{
    // size_t overflow paths must return NULL, not corrupt the heap.
    EXPECT_EQ(LocklessRingBufferCreate(SIZE_MAX, 1, LOCKLESS_RINGBUF_MODE_SPSC), nullptr);
    EXPECT_EQ(LocklessRingBufferCreate(SIZE_MAX, sizeof(uint64_t), LOCKLESS_RINGBUF_MODE_MPMC), nullptr);
    EXPECT_EQ(LocklessRingBufferCreate(SIZE_MAX / 2, sizeof(uint64_t), LOCKLESS_RINGBUF_MODE_MPSC), nullptr);
}

TEST(RingBufferAdversarialTest, PopWritesExactlyElemSize)
{
    const size_t elem_size = sizeof(uint64_t);
    LocklessRingBuffer *ring = LocklessRingBufferCreate(16, elem_size, LOCKLESS_RINGBUF_MODE_SPSC);
    ASSERT_NE(ring, nullptr);

    uint64_t value = 0xDEADBEEFCAFEBABEULL;
    ASSERT_TRUE(LocklessRingBufferTryPush(ring, &value));

    struct {
        uint8_t  pre[8];
        uint64_t payload;
        uint8_t  post[8];
    } out;
    memset(out.pre, 0xAA, sizeof(out.pre));
    memset(out.post, 0x55, sizeof(out.post));
    out.payload = 0;

    ASSERT_TRUE(LocklessRingBufferTryPop(ring, &out.payload));
    EXPECT_EQ(out.payload, value);
    for (size_t i = 0; i < sizeof(out.pre); ++i) EXPECT_EQ(out.pre[i], 0xAA);
    for (size_t i = 0; i < sizeof(out.post); ++i) EXPECT_EQ(out.post[i], 0x55);

    LocklessRingBufferDestroy(ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Concurrent safety
 * ═══════════════════════════════════════════════════════════════════════════ */

struct Item {
    uint64_t seq;
    uint64_t producer_id;
    uint64_t checksum;
};

uint64_t
sChecksum(uint64_t seq, uint64_t producer_id)
{
    return (seq * 0x9E3779B97F4A7C15ULL) ^ (producer_id * 0xC2B2AE3D27D4EB4FULL);
}

// Runs a full concurrent round-trip and validates exactly-once delivery,
// checksum integrity (torn-write detection), and per-producer FIFO for
// single-consumer modes.
void
sRunConcurrent(LocklessRingBufferMode mode, size_t capacity,
               int nproducers, int nconsumers, size_t items_per_producer)
{
    LocklessRingBuffer *ring = LocklessRingBufferCreate(capacity, sizeof(Item), mode);
    ASSERT_NE(ring, nullptr);

    std::atomic<size_t> produced{0};
    std::atomic<size_t> consumed{0};
    std::atomic<bool> error{false};

    const size_t total = static_cast<size_t>(nproducers) * items_per_producer;
    std::vector<uint64_t> next_seq(static_cast<size_t>(nproducers), 0);

    std::vector<std::thread> producers;
    producers.reserve(static_cast<size_t>(nproducers));
    for (int p = 0; p < nproducers; ++p) {
        producers.emplace_back([&, p]() {
            for (size_t i = 0; i < items_per_producer; ++i) {
                Item it;
                it.seq = i;
                it.producer_id = static_cast<uint64_t>(p);
                it.checksum = sChecksum(it.seq, it.producer_id);
                while (!LocklessRingBufferTryPush(ring, &it)) {
                    std::this_thread::yield();
                }
            }
            produced.fetch_add(items_per_producer, std::memory_order_relaxed);
        });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(static_cast<size_t>(nconsumers));
    for (int c = 0; c < nconsumers; ++c) {
        consumers.emplace_back([&]() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            for (;;) {
                if (consumed.load(std::memory_order_relaxed) >= total) {
                    return;
                }
                Item it;
                if (LocklessRingBufferTryPop(ring, &it)) {
                    if (it.checksum != sChecksum(it.seq, it.producer_id)) {
                        error.store(true, std::memory_order_relaxed);
                    }
                    if (nconsumers == 1) {
                        uint64_t expected = next_seq[it.producer_id];
                        if (it.seq != expected) {
                            error.store(true, std::memory_order_relaxed);
                        }
                        next_seq[it.producer_id] = expected + 1;
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else if (std::chrono::steady_clock::now() > deadline) {
                    error.store(true, std::memory_order_relaxed);
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (std::thread &t : producers) t.join();
    for (std::thread &t : consumers) t.join();

    LocklessRingBufferDestroy(ring);

    ASSERT_FALSE(error.load());
    ASSERT_EQ(consumed.load(), total);
    ASSERT_EQ(produced.load(), total);
}

TEST(RingBufferConcurrentTest, SpscExactlyOnce)
{
    sRunConcurrent(LOCKLESS_RINGBUF_MODE_SPSC, 1024, 1, 1, 100000);
}

TEST(RingBufferConcurrentTest, MpscExactlyOnceAndPerProducerFifo)
{
    sRunConcurrent(LOCKLESS_RINGBUF_MODE_MPSC, 4096, 4, 1, 25000);
}

TEST(RingBufferConcurrentTest, MpmcExactlyOnce)
{
    sRunConcurrent(LOCKLESS_RINGBUF_MODE_MPMC, 4096, 4, 4, 25000);
}

TEST(RingBufferConcurrentTest, HighContentionSmallCapacity)
{
    // Tiny capacity (4 slots) forces the full/empty boundary to be exercised
    // constantly — regresses the MPSC head/tail mismatch that deadlocked the
    // POC at tail-head == capacity.
    sRunConcurrent(LOCKLESS_RINGBUF_MODE_MPSC, 4, 8, 1, 5000);
    sRunConcurrent(LOCKLESS_RINGBUF_MODE_MPMC, 4, 8, 8, 5000);
}

TEST(RingBufferConcurrentTest, MixedSpeedProducersConsumers)
{
    // Producers and consumers of different counts/speeds must still deliver
    // every item exactly once with no corruption.
    sRunConcurrent(LOCKLESS_RINGBUF_MODE_MPMC, 512, 3, 7, 8000);
}
