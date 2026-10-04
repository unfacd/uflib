#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
#include <uflib/standard_c_includes.h>
#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack.h>
#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack_type.h>
}

namespace {

struct Node {
    struct LocklessTreiberStackNode base;
    int                            value;
};

constexpr int kConcurrentNodes = 8 * 512;

std::vector<Node> MakeNodes(int count)
{
    std::vector<Node> nodes((size_t)count);
    std::memset(nodes.data(), 0, sizeof(Node) * (size_t)count);
    for (int i = 0; i < count; i++) {
        lockless_treiber_stack_node_init(&nodes[(size_t)i].base);
        nodes[(size_t)i].value = i;
    }
    return nodes;
}

int ChainLength(struct LocklessTreiberStackNode *head)
{
    int n = 0;
    while (head != nullptr) {
        n++;
        head = head->next.load(std::memory_order_relaxed);
    }
    return n;
}

TEST(LocklessTreiberStackLifecycle, CreateDestroyEmpty)
{
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
    lockless_treiber_stack_destroy(s);

    LocklessTreiberStack *logged = lockless_treiber_stack_create_with_logger(nullptr);
    ASSERT_NE(logged, nullptr);
    lockless_treiber_stack_destroy(logged);

    lockless_treiber_stack_destroy(nullptr);
}

TEST(LocklessTreiberStackLifecycle, NullArgumentsDegradeGracefully)
{
    Node n;
    lockless_treiber_stack_node_init(&n.base);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    EXPECT_FALSE(lockless_treiber_stack_push(nullptr, &n.base));
    EXPECT_FALSE(lockless_treiber_stack_push(s, nullptr));
    EXPECT_FALSE(lockless_treiber_stack_push(nullptr, nullptr));
    EXPECT_EQ(lockless_treiber_stack_steal_all(nullptr), nullptr);
    EXPECT_EQ(lockless_treiber_stack_steal_all_and_close(nullptr), nullptr);

    lockless_treiber_stack_node_init(nullptr);
    lockless_treiber_stack_node_retain(nullptr);
    EXPECT_FALSE(lockless_treiber_stack_claim(nullptr));
    EXPECT_FALSE(lockless_treiber_stack_release(nullptr));

    EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
    lockless_treiber_stack_destroy(s);
}

TEST(LocklessTreiberStackNode, InitSetsDocumentedState)
{
    Node n;
    n.base.next.store((struct LocklessTreiberStackNode *)&n, std::memory_order_relaxed);
    n.base.is_claimed.store(true, std::memory_order_relaxed);
    n.base.refcount.store(99, std::memory_order_relaxed);

    lockless_treiber_stack_node_init(&n.base);

    EXPECT_EQ(n.base.next.load(std::memory_order_relaxed), nullptr);
    EXPECT_FALSE(n.base.is_claimed.load(std::memory_order_relaxed));
    EXPECT_EQ(n.base.refcount.load(std::memory_order_relaxed), 2);
}

TEST(LocklessTreiberStackNode, ClaimIsExactlyOnce)
{
    Node n;
    lockless_treiber_stack_node_init(&n.base);

    EXPECT_TRUE(lockless_treiber_stack_claim(&n.base));
    EXPECT_FALSE(lockless_treiber_stack_claim(&n.base));
    EXPECT_FALSE(lockless_treiber_stack_claim(&n.base));
    EXPECT_TRUE(n.base.is_claimed.load(std::memory_order_relaxed));
}

TEST(LocklessTreiberStackNode, ReleaseCountsDownToLastReference)
{
    Node n;
    lockless_treiber_stack_node_init(&n.base);

    EXPECT_FALSE(lockless_treiber_stack_release(&n.base));
    EXPECT_TRUE(lockless_treiber_stack_release(&n.base));
    EXPECT_EQ(n.base.refcount.load(std::memory_order_relaxed), 0);
}

TEST(LocklessTreiberStackNode, RetainAddsAReference)
{
    Node n;
    lockless_treiber_stack_node_init(&n.base);

    lockless_treiber_stack_node_retain(&n.base);
    EXPECT_EQ(n.base.refcount.load(std::memory_order_relaxed), 3);

    EXPECT_FALSE(lockless_treiber_stack_release(&n.base));
    EXPECT_FALSE(lockless_treiber_stack_release(&n.base));
    EXPECT_TRUE(lockless_treiber_stack_release(&n.base));
}

TEST(LocklessTreiberStackNode, RetainKeepsAClaimedNodeAlive)
{
    Node n;
    lockless_treiber_stack_node_init(&n.base);
    ASSERT_TRUE(lockless_treiber_stack_claim(&n.base));

    lockless_treiber_stack_node_retain(&n.base);
    EXPECT_FALSE(lockless_treiber_stack_release(&n.base));
    EXPECT_FALSE(lockless_treiber_stack_release(&n.base));
    EXPECT_TRUE(lockless_treiber_stack_release(&n.base));
}

TEST(LocklessTreiberStackDrain, LifoOrderAndChainIntegrity)
{
    constexpr int kCount = 64;
    std::vector<Node> nodes = MakeNodes(kCount);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    for (int i = 0; i < kCount; i++) {
        ASSERT_TRUE(lockless_treiber_stack_push(s, &nodes[(size_t)i].base));
    }

    struct LocklessTreiberStackNode *head = lockless_treiber_stack_steal_all(s);
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(ChainLength(head), kCount);

    for (int i = kCount - 1; i >= 0; i--) {
        ASSERT_NE(head, nullptr);
        EXPECT_EQ(head, &nodes[(size_t)i].base);
        head = head->next.load(std::memory_order_relaxed);
    }
    EXPECT_EQ(head, nullptr);

    EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
    lockless_treiber_stack_destroy(s);
}

TEST(LocklessTreiberStackDrain, DrainThenReuseLosesNothing)
{
    constexpr int kCount = 32;
    std::vector<Node> nodes = MakeNodes(kCount * 8);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < kCount; i++) {
            ASSERT_TRUE(lockless_treiber_stack_push(s, &nodes[(size_t)(round * kCount + i)].base));
        }
        EXPECT_EQ(ChainLength(lockless_treiber_stack_steal_all(s)), kCount);
        EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
    }

    lockless_treiber_stack_destroy(s);
}

TEST(LocklessTreiberStackDrain, RepeatedDrainOfOneNodeIsStable)
{
    Node n;
    lockless_treiber_stack_node_init(&n.base);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    ASSERT_TRUE(lockless_treiber_stack_push(s, &n.base));
    EXPECT_EQ(lockless_treiber_stack_steal_all(s), &n.base);
    EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
    EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);

    lockless_treiber_stack_destroy(s);
}

TEST(LocklessTreiberStackClosed, CloseReturnsTheChainAndRefusesFurtherPushes)
{
    constexpr int kCount = 16;
    std::vector<Node> nodes = MakeNodes(kCount + 1);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    for (int i = 0; i < kCount; i++) {
        ASSERT_TRUE(lockless_treiber_stack_push(s, &nodes[(size_t)i].base));
    }

    struct LocklessTreiberStackNode *head = lockless_treiber_stack_steal_all_and_close(s);
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(head, &nodes[kCount - 1].base);
    EXPECT_EQ(ChainLength(head), kCount);

    EXPECT_FALSE(lockless_treiber_stack_push(s, &nodes[kCount].base));
    EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
    EXPECT_EQ(lockless_treiber_stack_steal_all_and_close(s), nullptr);
    EXPECT_FALSE(lockless_treiber_stack_push(s, &nodes[kCount].base));

    lockless_treiber_stack_destroy(s);
}

TEST(LocklessTreiberStackClosed, CloseOnEmptyStackIsIdempotentAndFinal)
{
    Node n;
    lockless_treiber_stack_node_init(&n.base);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    EXPECT_EQ(lockless_treiber_stack_steal_all_and_close(s), nullptr);
    EXPECT_EQ(lockless_treiber_stack_steal_all_and_close(s), nullptr);
    EXPECT_FALSE(lockless_treiber_stack_push(s, &n.base));
    EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
    EXPECT_FALSE(lockless_treiber_stack_push(s, &n.base));

    lockless_treiber_stack_destroy(s);
}

TEST(LocklessTreiberStackClosed, StealAllDoesNotReopenAClosedStack)
{
    Node n;
    lockless_treiber_stack_node_init(&n.base);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    ASSERT_EQ(lockless_treiber_stack_steal_all_and_close(s), nullptr);
    for (int i = 0; i < 8; i++) {
        EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
        EXPECT_FALSE(lockless_treiber_stack_push(s, &n.base));
    }

    lockless_treiber_stack_destroy(s);
}

TEST(LocklessTreiberStackClosed, RejectedPushLeavesTheNodeUnlinked)
{
    Node n;
    lockless_treiber_stack_node_init(&n.base);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    ASSERT_EQ(lockless_treiber_stack_steal_all_and_close(s), nullptr);
    ASSERT_FALSE(lockless_treiber_stack_push(s, &n.base));
    EXPECT_EQ(n.base.next.load(std::memory_order_relaxed), nullptr);
    EXPECT_EQ(n.base.refcount.load(std::memory_order_relaxed), 2);

    lockless_treiber_stack_destroy(s);
}

#ifndef NDEBUG
TEST(LocklessTreiberStackClosed, DestroyWithLeftoversAborts)
{
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    Node n;
    lockless_treiber_stack_node_init(&n.base);
    ASSERT_TRUE(lockless_treiber_stack_push(s, &n.base));

    EXPECT_DEATH(lockless_treiber_stack_destroy(s), "");

    EXPECT_EQ(lockless_treiber_stack_steal_all(s), &n.base);
    lockless_treiber_stack_destroy(s);
}
#endif

TEST(LocklessTreiberStackThreads, ConcurrentProducersSingleDrainLosesNothing)
{
    std::vector<Node> nodes = MakeNodes(kConcurrentNodes);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    constexpr int kThreads = 8;
    constexpr int kPerThread = kConcurrentNodes / kThreads;
    std::atomic<int> refused{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; i++) {
                Node *n = &nodes[(size_t)(t * kPerThread + i)];
                if (!lockless_treiber_stack_push(s, &n->base)) {
                    refused.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread &th : threads) {
        th.join();
    }

    EXPECT_EQ(refused.load(), 0);
    EXPECT_EQ(ChainLength(lockless_treiber_stack_steal_all(s)), kConcurrentNodes);
    EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
    lockless_treiber_stack_destroy(s);
}

TEST(LocklessTreiberStackThreads, ProducerConsumerAccountingIsExact)
{
    constexpr int kRounds = 40;
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 512;
    constexpr int kTotal = kProducers * kPerProducer;

    for (int round = 0; round < kRounds; round++) {
        std::vector<Node> nodes = MakeNodes(kTotal);
        std::vector<int> seen((size_t)kTotal, 0);
        LocklessTreiberStack *s = lockless_treiber_stack_create();
        ASSERT_NE(s, nullptr);

        std::atomic<bool> producers_done{false};
        std::atomic<int> drained{0};
        std::atomic<int> duplicates{0};
        std::atomic<int> out_of_range{0};

        std::thread consumer([&]() {
            for (;;) {
                struct LocklessTreiberStackNode *head = lockless_treiber_stack_steal_all(s);
                if (head == nullptr) {
                    if (producers_done.load(std::memory_order_acquire)) {
                        break;
                    }
                    std::this_thread::yield();
                    continue;
                }
                while (head != nullptr) {
                    struct LocklessTreiberStackNode *next =
                        head->next.load(std::memory_order_relaxed);
                    Node *n = reinterpret_cast<Node *>(head);
                    if (n->value < 0 || n->value >= kTotal) {
                        out_of_range.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        if (seen[(size_t)n->value]++ != 0) {
                            duplicates.fetch_add(1, std::memory_order_relaxed);
                        }
                        drained.fetch_add(1, std::memory_order_relaxed);
                    }
                    head = next;
                }
            }
        });

        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (int p = 0; p < kProducers; p++) {
            producers.emplace_back([&, p]() {
                for (int i = 0; i < kPerProducer; i++) {
                    Node *n = &nodes[(size_t)(p * kPerProducer + i)];
                    while (!lockless_treiber_stack_push(s, &n->base)) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (std::thread &th : producers) {
            th.join();
        }
        producers_done.store(true, std::memory_order_release);
        consumer.join();

        EXPECT_EQ(out_of_range.load(), 0);
        EXPECT_EQ(duplicates.load(), 0);
        EXPECT_EQ(drained.load(), kTotal);
        int missing = 0;
        for (int i = 0; i < kTotal; i++) {
            if (seen[(size_t)i] != 1) {
                missing++;
            }
        }
        EXPECT_EQ(missing, 0);
        EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
        lockless_treiber_stack_destroy(s);
    }
}

TEST(LocklessTreiberStackThreads, PushRacingCloseAccountsForEveryNode)
{
    constexpr int kRounds = 200;
    constexpr int kProducers = 8;

    for (int round = 0; round < kRounds; round++) {
        std::vector<Node> nodes = MakeNodes(kProducers);
        std::atomic<int> accepted[kProducers];
        for (int i = 0; i < kProducers; i++) {
            accepted[i].store(0, std::memory_order_relaxed);
        }
        LocklessTreiberStack *s = lockless_treiber_stack_create();
        ASSERT_NE(s, nullptr);

        std::vector<std::thread> threads;
        threads.reserve(kProducers);
        for (int t = 0; t < kProducers; t++) {
            threads.emplace_back([&, t]() {
                if (lockless_treiber_stack_push(s, &nodes[(size_t)t].base)) {
                    accepted[t].store(1, std::memory_order_relaxed);
                }
            });
        }

        struct LocklessTreiberStackNode *head = lockless_treiber_stack_steal_all_and_close(s);
        for (std::thread &th : threads) {
            th.join();
        }

        int in_chain = 0;
        int accepted_count = 0;
        int foreign = 0;
        while (head != nullptr) {
            struct LocklessTreiberStackNode *next =
                head->next.load(std::memory_order_relaxed);
            Node *n = reinterpret_cast<Node *>(head);
            if (n->value < 0 || n->value >= kProducers) {
                foreign++;
            } else {
                EXPECT_EQ(accepted[n->value].load(), 1);
            }
            in_chain++;
            head = next;
        }
        EXPECT_EQ(foreign, 0);
        for (int i = 0; i < kProducers; i++) {
            accepted_count += accepted[i].load();
        }
        EXPECT_EQ(in_chain, accepted_count);

        EXPECT_FALSE(lockless_treiber_stack_push(s, &nodes[0].base));
        lockless_treiber_stack_destroy(s);
    }
}

TEST(LocklessTreiberStackThreads, ClaimRaceHasExactlyOneWinner)
{
    constexpr int kRounds = 256;
    constexpr int kThreads = 8;

    for (int round = 0; round < kRounds; round++) {
        Node n;
        lockless_treiber_stack_node_init(&n.base);
        std::atomic<int> winners{0};

        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; t++) {
            threads.emplace_back([&]() {
                if (lockless_treiber_stack_claim(&n.base)) {
                    winners.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (std::thread &th : threads) {
            th.join();
        }
        EXPECT_EQ(winners.load(), 1);
    }
}

TEST(LocklessTreiberStackThreads, ReleaseRaceHasExactlyOneLastReference)
{
    constexpr int kRounds = 256;
    constexpr int kThreads = 8;

    for (int round = 0; round < kRounds; round++) {
        Node n;
        lockless_treiber_stack_node_init(&n.base);
        for (int i = 0; i < kThreads; i++) {
            lockless_treiber_stack_node_retain(&n.base);
        }
        constexpr int kReleasers = kThreads + 2;
        std::atomic<int> last{0};

        std::vector<std::thread> threads;
        threads.reserve(kReleasers);
        for (int t = 0; t < kReleasers; t++) {
            threads.emplace_back([&]() {
                if (lockless_treiber_stack_release(&n.base)) {
                    last.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (std::thread &th : threads) {
            th.join();
        }
        EXPECT_EQ(last.load(), 1);
        EXPECT_EQ(n.base.refcount.load(std::memory_order_relaxed), 0);
    }
}

TEST(LocklessTreiberStackThreads, SingleConsumerDrainsEverythingPushed)
{
    std::vector<Node> nodes = MakeNodes(4096);
    LocklessTreiberStack *s = lockless_treiber_stack_create();
    ASSERT_NE(s, nullptr);

    for (int i = 0; i < 4096; i++) {
        ASSERT_TRUE(lockless_treiber_stack_push(s, &nodes[(size_t)i].base));
    }

    std::atomic<int> drained{0};
    std::thread consumer([&]() {
        for (;;) {
            struct LocklessTreiberStackNode *head = lockless_treiber_stack_steal_all(s);
            if (head == nullptr) {
                break;
            }
            drained.fetch_add(ChainLength(head), std::memory_order_relaxed);
        }
    });
    consumer.join();

    EXPECT_EQ(drained.load(), 4096);
    EXPECT_EQ(lockless_treiber_stack_steal_all(s), nullptr);
    lockless_treiber_stack_destroy(s);
}

}  // namespace
