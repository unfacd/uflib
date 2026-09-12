/**
 * @file cdt_chase_lev_stealing_queue_tests.cpp
 * @brief GoogleTest suite for the Chase–Lev work-stealing deque
 *        (ChaseLevStealingQueue).
 *
 * Covers lifecycle, power-of-two rounding, sequential LIFO pop and FIFO steal
 * across many grows from capacity 2, the owner-last-item case, the NULL
 * contract, ApproxSize, the concurrent last-item duel (owner pop vs thieves,
 * exactly-once), and the private cache-line layout lock.
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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

extern "C" {
#include <uflib/cdt/chase_lev_stealing_queue/cdt_chase_lev_stealing_queue.h>
#include "cdt_chase_lev_stealing_queue_priv.h"
}

namespace {

/* ── Lifecycle & rounding ───────────────────────────────────────────────── */

TEST(ChaseLevStealingQueueTest, CreateDestroy) {
    ChaseLevStealingQueue *q = ChaseLevStealingQueueCreate(64);
    ASSERT_NE(q, nullptr);
    ChaseLevStealingQueueDestroy(q);
}

TEST(ChaseLevStealingQueueTest, CapacityRounding) {
    ChaseLevStealingQueue *q = ChaseLevStealingQueueCreate(0);
    ASSERT_NE(q, nullptr);
    EXPECT_GE(ChaseLevStealingQueueCapacity(q), 2u);
    ChaseLevStealingQueueDestroy(q);

    q = ChaseLevStealingQueueCreate(3);
    ASSERT_NE(q, nullptr);
    EXPECT_GE(ChaseLevStealingQueueCapacity(q), 4u);
    ChaseLevStealingQueueDestroy(q);

    q = ChaseLevStealingQueueCreate(5);
    ASSERT_NE(q, nullptr);
    EXPECT_GE(ChaseLevStealingQueueCapacity(q), 8u);
    ChaseLevStealingQueueDestroy(q);

    q = ChaseLevStealingQueueCreate(1024);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(ChaseLevStealingQueueCapacity(q), 1024u);
    ChaseLevStealingQueueDestroy(q);
}

/* ── Sequential semantics ───────────────────────────────────────────────── */

TEST(ChaseLevStealingQueueTest, LifoPopAcrossGrows) {
    constexpr uintptr_t N = 100000;
    ChaseLevStealingQueue *q = ChaseLevStealingQueueCreate(2);
    ASSERT_NE(q, nullptr);

    for (uintptr_t i = 1; i <= N; ++i) {
        ASSERT_TRUE(ChaseLevStealingQueuePush(q, (void *)i));
    }
    EXPECT_EQ(ChaseLevStealingQueueApproxSize(q), (uint64_t)N);

    for (uintptr_t i = N; i > 0; --i) {
        void *p = nullptr;
        ASSERT_TRUE(ChaseLevStealingQueuePop(q, &p));
        EXPECT_EQ((uintptr_t)p, i);
    }
    EXPECT_EQ(ChaseLevStealingQueueApproxSize(q), 0u);
    ChaseLevStealingQueueDestroy(q);
}

TEST(ChaseLevStealingQueueTest, FifoStealAcrossGrows) {
    constexpr uintptr_t N = 100000;
    ChaseLevStealingQueue *q = ChaseLevStealingQueueCreate(2);
    ASSERT_NE(q, nullptr);

    for (uintptr_t i = 1; i <= N; ++i) {
        ASSERT_TRUE(ChaseLevStealingQueuePush(q, (void *)i));
    }

    for (uintptr_t i = 1; i <= N; ++i) {
        void *p = nullptr;
        ASSERT_TRUE(ChaseLevStealingQueueSteal(q, &p));
        EXPECT_EQ((uintptr_t)p, i);
    }
    ChaseLevStealingQueueDestroy(q);
}

TEST(ChaseLevStealingQueueTest, OwnerLastItemAlone) {
    ChaseLevStealingQueue *q = ChaseLevStealingQueueCreate(2);
    ASSERT_NE(q, nullptr);

    ASSERT_TRUE(ChaseLevStealingQueuePush(q, (void *)42u));
    void *p = nullptr;
    ASSERT_TRUE(ChaseLevStealingQueuePop(q, &p));
    EXPECT_EQ((uintptr_t)p, 42u);
    EXPECT_FALSE(ChaseLevStealingQueuePop(q, &p));
    ChaseLevStealingQueueDestroy(q);
}

TEST(ChaseLevStealingQueueTest, NullContract) {
    ChaseLevStealingQueue *q = ChaseLevStealingQueueCreate(2);
    ASSERT_NE(q, nullptr);

    void *p = nullptr;
    EXPECT_FALSE(ChaseLevStealingQueuePush(q, nullptr));     /* NULL is not an item */
    EXPECT_FALSE(ChaseLevStealingQueuePush(nullptr, (void *)1u));
    EXPECT_FALSE(ChaseLevStealingQueuePop(nullptr, &p));
    EXPECT_FALSE(ChaseLevStealingQueuePop(q, nullptr));
    EXPECT_FALSE(ChaseLevStealingQueueSteal(nullptr, &p));
    EXPECT_FALSE(ChaseLevStealingQueueSteal(q, nullptr));
    EXPECT_EQ(ChaseLevStealingQueueCapacity(nullptr), 0u);
    EXPECT_EQ(ChaseLevStealingQueueApproxSize(nullptr), 0u);

    ChaseLevStealingQueueDestroy(q);
    ChaseLevStealingQueueDestroy(nullptr);
}

/* ── Concurrent last-item duel ──────────────────────────────────────────── */

namespace {

struct LastItemCtx {
    ChaseLevStealingQueue *q;
    std::atomic<unsigned> stop{0};
    std::atomic<unsigned> thief_wins{0};
    std::atomic<unsigned> bad{0};
    std::unique_ptr<std::atomic<unsigned>[]> seen;
    unsigned trials;
};

void LastItemThief(LastItemCtx *c) {
    for (;;) {
        void *p = nullptr;
        if (ChaseLevStealingQueueSteal(c->q, &p)) {
            const uintptr_t id = (uintptr_t)p;
            if (id == 0u || id > (uintptr_t)c->trials) {
                c->bad.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const unsigned prev = c->seen[id - 1u].fetch_add(1, std::memory_order_relaxed);
            if (prev != 0u) {
                c->bad.fetch_add(1, std::memory_order_relaxed);
            }
            c->thief_wins.fetch_add(1, std::memory_order_relaxed);
        } else if (c->stop.load(std::memory_order_acquire) != 0u) {
            break;
        } else {
            std::this_thread::yield();
        }
    }
}

}  // namespace

TEST(ChaseLevStealingQueueTest, LastItemRaceExactlyOnce) {
    constexpr unsigned TRIALS = 20000;
    constexpr unsigned THIEVES = 4;

    LastItemCtx c;
    c.q = ChaseLevStealingQueueCreate(2);
    ASSERT_NE(c.q, nullptr);
    c.trials = TRIALS;
    c.seen.reset(new std::atomic<unsigned>[TRIALS]());

    std::vector<std::thread> thieves;
    for (unsigned i = 0; i < THIEVES; ++i) {
        thieves.emplace_back(LastItemThief, &c);
    }

    unsigned owner_wins = 0u;
    for (unsigned trial = 1u; trial <= TRIALS; ++trial) {
        ASSERT_TRUE(ChaseLevStealingQueuePush(c.q, (void *)(uintptr_t)trial));
        std::this_thread::yield();
        void *p = nullptr;
        if (ChaseLevStealingQueuePop(c.q, &p)) {
            EXPECT_EQ((uintptr_t)p, (uintptr_t)trial);
            const unsigned prev = c.seen[trial - 1u].fetch_add(1, std::memory_order_relaxed);
            EXPECT_EQ(prev, 0u);
            ++owner_wins;
        }
    }

    c.stop.store(1, std::memory_order_release);
    for (auto &t : thieves) {
        t.join();
    }

    unsigned missing = 0u;
    for (unsigned i = 0u; i < TRIALS; ++i) {
        if (c.seen[i].load(std::memory_order_relaxed) != 1u) {
            ++missing;
        }
    }

    const unsigned thief_wins = c.thief_wins.load(std::memory_order_relaxed);
    const unsigned bad = c.bad.load(std::memory_order_relaxed);

    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(missing, 0u);
    EXPECT_EQ(owner_wins + thief_wins, TRIALS);

    ChaseLevStealingQueueDestroy(c.q);
}

/* ── Private layout lock ────────────────────────────────────────────────── */

TEST(ChaseLevStealingQueueTest, CacheLineLayout) {
    /* The four contended fields must each sit on a distinct cache line; the
     * owner-only fields must follow.  Guards the false-sharing design. */
    EXPECT_EQ(offsetof(struct ChaseLevStealingQueue, top), 0u);
    EXPECT_EQ(offsetof(struct ChaseLevStealingQueue, bottom),
              (size_t)UFLIB_CDT_CACHE_LINE_SIZE);
    EXPECT_EQ(offsetof(struct ChaseLevStealingQueue, array),
              2u * (size_t)UFLIB_CDT_CACHE_LINE_SIZE);
    EXPECT_EQ(offsetof(struct ChaseLevStealingQueue, capacity),
              3u * (size_t)UFLIB_CDT_CACHE_LINE_SIZE);
    /* retired/cached_top are owner-only and sit after the four lines. */
    EXPECT_GT(offsetof(struct ChaseLevStealingQueue, retired),
              3u * (size_t)UFLIB_CDT_CACHE_LINE_SIZE);
    EXPECT_GT(offsetof(struct ChaseLevStealingQueue, cached_top),
              3u * (size_t)UFLIB_CDT_CACHE_LINE_SIZE);
}

}  // namespace
