// AliasMap unit tests — public API only.

extern "C" {
#include <uflib/cdt/alias_map/alias_map.h>
}

#include <uflib/logger/logger.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string
Key(int index)
{
    return "k" + std::to_string(index);
}

std::string
Value(int index)
{
    return "v" + std::to_string(index);
}

// Sorted group snapshot as strings; empty when the alias is absent.
std::vector<std::string>
Group(AliasMap *map_ptr, const char *alias_ptr)
{
    size_t count = 0;
    char **list  = AliasMapAliasesOf(map_ptr, alias_ptr, &count);

    std::vector<std::string> out;

    if (!list) return out;

    out.reserve(count);

    for (size_t i = 0; i < count; i++) out.emplace_back(list[i]);

    std::sort(out.begin(), out.end());
    free(list);

    return out;
}

void
ExpectValue(AliasMap *map_ptr, const char *alias_ptr, const char *want_ptr)
{
    char *value = AliasMapGetCopy(map_ptr, alias_ptr);

    ASSERT_NE(value, nullptr) << "alias '" << alias_ptr << "'";
    EXPECT_STREQ(value, want_ptr);

    free(value);
}

void
ExpectAbsent(AliasMap *map_ptr, const char *alias_ptr)
{
    EXPECT_FALSE(AliasMapContains(map_ptr, alias_ptr));
    EXPECT_EQ(AliasMapGetCopy(map_ptr, alias_ptr), nullptr);
}

// Structural invariants over the key pool k0..k(nkeys-1).  Only valid when
// every alias in the map is one of those keys and nobody else is running.
void
CheckInvariants(AliasMap *map_ptr, int key_count)
{
    size_t found = 0;

    for (int i = 0; i < key_count; i++) {
        std::string key  = Key(i);
        auto        list = Group(map_ptr, key.c_str());

        EXPECT_EQ(AliasMapContains(map_ptr, key.c_str()), !list.empty());

        if (list.empty()) continue;

        found++;

        char *value = AliasMapGetCopy(map_ptr, key.c_str());
        ASSERT_NE(value, nullptr);

        for (size_t j = 0; j < list.size(); j++) {
            EXPECT_NE(std::find(list.begin(), list.end(), key), list.end());

            for (size_t q = 0; q < j; q++) EXPECT_NE(list[q], list[j]);

            char *other = AliasMapGetCopy(map_ptr, list[j].c_str());
            ASSERT_NE(other, nullptr);
            EXPECT_STREQ(other, value);
            free(other);

            auto symmetric = Group(map_ptr, list[j].c_str());
            EXPECT_EQ(symmetric, list);
        }

        free(value);
    }

    EXPECT_EQ(found, AliasMapAliasCount(map_ptr));
}

class AliasMapTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        map_ptr = AliasMapCreate();
        ASSERT_NE(map_ptr, nullptr);
    }

    void TearDown() override
    {
        AliasMapDestroy(map_ptr);
    }

    AliasMap *map_ptr = nullptr;
};

using AliasMapLifecycleTest = AliasMapTest;

TEST_F(AliasMapLifecycleTest, CreateReturnsUsableMap)
{
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 0u);
    EXPECT_FALSE(AliasMapContains(map_ptr, "anything"));
}

TEST_F(AliasMapLifecycleTest, DestroyNullIsNoOp)
{
    AliasMapDestroy(nullptr);
}

TEST_F(AliasMapLifecycleTest, DestroyFreesLiveGroups)
{
    const char *group[] = { "a", "b" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 2, "value"));

    AliasMapDestroy(map_ptr);
    map_ptr = nullptr; // TearDown destroys null
}

/* ── Add ───────────────────────────────────────────────────────────────── */

TEST_F(AliasMapTest, AddCreatesOneGroupPerDocument)
{
    const char *group[] = { "ll", "lsl", "longlist" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 3, "ls -la"));
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 3u);

    ExpectValue(map_ptr, "ll", "ls -la");
    ExpectValue(map_ptr, "lsl", "ls -la");
    ExpectValue(map_ptr, "longlist", "ls -la");
    ExpectAbsent(map_ptr, "nope");

    EXPECT_EQ(Group(map_ptr, "lsl"), (std::vector<std::string>{"ll", "longlist", "lsl"}));
}

TEST_F(AliasMapTest, AddRejectsExistingAliasAndChangesNothing)
{
    const char *first[]  = { "a" };
    const char *second[] = { "b", "a" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, first, 1, "one"));
    ASSERT_FALSE(AliasMapAdd(map_ptr, second, 2, "two"));

    ExpectValue(map_ptr, "a", "one");
    ExpectAbsent(map_ptr, "b");
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 1u);
}

TEST_F(AliasMapTest, AddRejectsDuplicateWithinList)
{
    const char *group[] = { "d", "d" };

    EXPECT_FALSE(AliasMapAdd(map_ptr, group, 2, "value"));
    ExpectAbsent(map_ptr, "d");
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 0u);
}

TEST_F(AliasMapTest, AddRejectsInvalidArguments)
{
    const char *group[] = { "a" };

    EXPECT_FALSE(AliasMapAdd(nullptr, group, 1, "value"));
    EXPECT_FALSE(AliasMapAdd(map_ptr, nullptr, 1, "value"));
    EXPECT_FALSE(AliasMapAdd(map_ptr, group, 0, "value"));
    EXPECT_FALSE(AliasMapAdd(map_ptr, group, 1, nullptr));

    const char *with_null[] = { "a", nullptr };

    EXPECT_FALSE(AliasMapAdd(map_ptr, with_null, 2, "value"));
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 0u);
}

TEST_F(AliasMapTest, AddAcceptsEmptyAliasAndEmptyValue)
{
    const char *empty_alias[] = { "" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, empty_alias, 1, ""));
    EXPECT_TRUE(AliasMapContains(map_ptr, ""));
    ExpectValue(map_ptr, "", "");

    char   buffer[4] = { 'x', 'x', 'x', 'x' };
    size_t length    = 99;

    ASSERT_TRUE(AliasMapGetInto(map_ptr, "", buffer, sizeof buffer, &length));
    EXPECT_EQ(length, 0u);
    EXPECT_EQ(buffer[0], '\0');
}

/* ── AddAlias ──────────────────────────────────────────────────────────── */

TEST_F(AliasMapTest, AddAliasExtendsGroup)
{
    const char *group[] = { "ll", "lsl", "longlist" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 3, "ls -la"));
    ASSERT_TRUE(AliasMapAddAlias(map_ptr, "ll", "dirlist"));

    ExpectValue(map_ptr, "dirlist", "ls -la");
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 4u);

    // Not idempotent, and a present alias fails even inside the same group.
    EXPECT_FALSE(AliasMapAddAlias(map_ptr, "ll", "dirlist"));
    EXPECT_FALSE(AliasMapAddAlias(map_ptr, "missing", "x"));
    EXPECT_FALSE(AliasMapAddAlias(map_ptr, "ll", "lsl"));
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 4u);
}

TEST_F(AliasMapTest, AddAliasRejectsNullArguments)
{
    const char *group[] = { "ll" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "value"));

    EXPECT_FALSE(AliasMapAddAlias(nullptr, "ll", "dirlist"));
    EXPECT_FALSE(AliasMapAddAlias(map_ptr, nullptr, "dirlist"));
    EXPECT_FALSE(AliasMapAddAlias(map_ptr, "ll", nullptr));
}

/* ── Lookup ────────────────────────────────────────────────────────────── */

TEST_F(AliasMapTest, GetCopyAndGetInto)
{
    const char *group[] = { "greeting" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "/opt/etc/ufsrv/assets/greeting.txt"));

    char   buffer[64];
    size_t length = 0;

    ASSERT_TRUE(AliasMapGetInto(map_ptr, "greeting", buffer, sizeof buffer, &length));
    EXPECT_STREQ(buffer, "/opt/etc/ufsrv/assets/greeting.txt");
    EXPECT_EQ(length, std::strlen("/opt/etc/ufsrv/assets/greeting.txt"));
}

TEST_F(AliasMapTest, GetIntoReportsErrors)
{
    const char *group[] = { "greeting" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "payload"));

    char   small[4] = { 'x', 'x', 'x', 'x' };
    size_t length   = 99;

    errno = 0;
    EXPECT_FALSE(AliasMapGetInto(map_ptr, "missing", small, sizeof small, &length));
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(length, 0u);

    errno = 0;
    EXPECT_FALSE(AliasMapGetInto(map_ptr, "greeting", small, sizeof small, &length));
    EXPECT_EQ(errno, ENOSPC);
    EXPECT_EQ(small[0], 'x'); // untouched

    errno = 0;
    EXPECT_FALSE(AliasMapGetInto(map_ptr, "greeting", nullptr, sizeof small, &length));
    EXPECT_EQ(errno, EINVAL);

    errno = 0;
    EXPECT_FALSE(AliasMapGetInto(map_ptr, "greeting", small, 0, &length));
    EXPECT_EQ(errno, EINVAL);

    EXPECT_FALSE(AliasMapGetInto(nullptr, "greeting", small, sizeof small, &length));
}

TEST_F(AliasMapTest, ContainsRejectsNullArguments)
{
    EXPECT_FALSE(AliasMapContains(nullptr, "x"));
    EXPECT_FALSE(AliasMapContains(map_ptr, nullptr));
    EXPECT_FALSE(AliasMapContains(map_ptr, "x"));
}

/* ── SetValue / Remove ─────────────────────────────────────────────────── */

TEST_F(AliasMapTest, SetValueUpdatesWholeGroup)
{
    const char *group[] = { "ll", "longlist" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 2, "ls -la"));
    ASSERT_TRUE(AliasMapSetValue(map_ptr, "longlist", "ls -lah"));

    ExpectValue(map_ptr, "ll", "ls -lah");
    ExpectValue(map_ptr, "longlist", "ls -lah");

    EXPECT_FALSE(AliasMapSetValue(map_ptr, "missing", "x"));
    EXPECT_FALSE(AliasMapSetValue(nullptr, "ll", "x"));
    EXPECT_FALSE(AliasMapSetValue(map_ptr, "ll", nullptr));
}

TEST_F(AliasMapTest, RemoveSingleAliasKeepsGroup)
{
    const char *group[] = { "ll", "lsl", "longlist" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 3, "ls -la"));
    ASSERT_TRUE(AliasMapRemove(map_ptr, "lsl"));

    ExpectAbsent(map_ptr, "lsl");
    ExpectValue(map_ptr, "ll", "ls -la");
    ExpectValue(map_ptr, "longlist", "ls -la");
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 2u);

    EXPECT_FALSE(AliasMapRemove(map_ptr, "lsl"));
}

TEST_F(AliasMapTest, RemoveLastAliasFreesGroup)
{
    const char *group[] = { "solo" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "value"));
    ASSERT_TRUE(AliasMapRemove(map_ptr, "solo"));

    ExpectAbsent(map_ptr, "solo");
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 0u);
}

TEST_F(AliasMapTest, RemoveValueRemovesWholeGroup)
{
    const char *first[]  = { "gs", "status" };
    const char *second[] = { "ll" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, first, 2, "git status"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, second, 1, "ls -la"));

    EXPECT_EQ(AliasMapRemoveValue(map_ptr, "status"), 2u);
    ExpectAbsent(map_ptr, "gs");
    ExpectAbsent(map_ptr, "status");
    ExpectValue(map_ptr, "ll", "ls -la");

    EXPECT_EQ(AliasMapRemoveValue(map_ptr, "nope"), 0u);
    EXPECT_EQ(AliasMapRemoveValue(nullptr, "ll"), 0u);
    EXPECT_EQ(AliasMapRemoveValue(map_ptr, nullptr), 0u);
}

/* ── AliasesOf ─────────────────────────────────────────────────────────── */

TEST_F(AliasMapTest, AliasesOfSnapshotLayoutAndOrder)
{
    const char *group[] = { "ll", "lsl", "longlist" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 3, "ls -la"));
    ASSERT_TRUE(AliasMapAddAlias(map_ptr, "ll", "dirlist"));

    size_t count = 0;
    char **list  = AliasMapAliasesOf(map_ptr, "lsl", &count);

    ASSERT_NE(list, nullptr);
    EXPECT_EQ(count, 4u);

    // Most recently attached first, self included.
    EXPECT_STREQ(list[0], "dirlist");

    // Strings live inside the same allocation as the pointer array.
    EXPECT_GE(reinterpret_cast<char *>(list[0]), reinterpret_cast<char *>(list + count));

    free(list);
}

TEST_F(AliasMapTest, AliasesOfSnapshotIsIndependent)
{
    const char *group[] = { "ll", "lsl", "longlist" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 3, "ls -la"));

    size_t count = 0;
    char **list  = AliasMapAliasesOf(map_ptr, "ll", &count);

    ASSERT_NE(list, nullptr);
    EXPECT_EQ(AliasMapRemoveValue(map_ptr, "ll"), 3u);

    std::vector<std::string> snapshot;

    for (size_t i = 0; i < count; i++) snapshot.emplace_back(list[i]);

    std::sort(snapshot.begin(), snapshot.end());

    EXPECT_EQ(snapshot, (std::vector<std::string>{"ll", "longlist", "lsl"}));

    free(list);
    EXPECT_EQ(AliasMapAliasesOf(map_ptr, "ll", &count), nullptr);
    EXPECT_EQ(count, 0u);
}

TEST_F(AliasMapTest, AliasesOfRejectsInvalidArguments)
{
    size_t count = 99;

    EXPECT_EQ(AliasMapAliasesOf(nullptr, "x", &count), nullptr);
    EXPECT_EQ(count, 0u);

    count = 99;
    EXPECT_EQ(AliasMapAliasesOf(map_ptr, nullptr, &count), nullptr);
    EXPECT_EQ(count, 0u);

    const char *group[] = { "ll" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "value"));
    EXPECT_EQ(AliasMapAliasesOf(map_ptr, "missing", &count), nullptr);
    EXPECT_EQ(count, 0u);
}

/* ── Replace ───────────────────────────────────────────────────────────── */

TEST_F(AliasMapTest, ReplaceStealsMovesAndCreates)
{
    const char *g1[]  = { "a", "b", "c" };
    const char *g2[]  = { "d", "e" };
    const char *g3[]  = { "f" };
    const char *replace[] = { "c", "e", "f", "n1", "n2" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, g1, 3, "v1"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, g2, 2, "v2"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, g3, 1, "v3"));

    ASSERT_TRUE(AliasMapReplace(map_ptr, replace, 5, "nv"));

    for (const char *name : replace) ExpectValue(map_ptr, name, "nv");

    ExpectValue(map_ptr, "a", "v1");
    ExpectValue(map_ptr, "b", "v1");
    ExpectValue(map_ptr, "d", "v2");
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 8u);

    EXPECT_EQ(Group(map_ptr, "a"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(Group(map_ptr, "d"), (std::vector<std::string>{"d"}));
    EXPECT_EQ(Group(map_ptr, "c"),
              (std::vector<std::string>{"c", "e", "f", "n1", "n2"}));
}

TEST_F(AliasMapTest, ReplaceRejectsDuplicatesAndInvalidArguments)
{
    const char *group[] = { "a", "b" };
    const char *duplicates[] = { "a", "x", "a" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 2, "v"));

    EXPECT_FALSE(AliasMapReplace(map_ptr, duplicates, 3, "bad"));
    ExpectAbsent(map_ptr, "x");
    ExpectValue(map_ptr, "a", "v");

    EXPECT_FALSE(AliasMapReplace(map_ptr, group, 0, "v"));
    EXPECT_FALSE(AliasMapReplace(map_ptr, nullptr, 1, "v"));
    EXPECT_FALSE(AliasMapReplace(map_ptr, group, 2, nullptr));
    EXPECT_FALSE(AliasMapReplace(nullptr, group, 2, "v"));

    const char *with_null[] = { "a", nullptr };

    EXPECT_FALSE(AliasMapReplace(map_ptr, with_null, 2, "v"));
    ExpectValue(map_ptr, "a", "v");
}

TEST_F(AliasMapTest, ReplaceMovesWholeGroupAndSingleton)
{
    const char *group[] = { "a", "b" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 2, "v1"));

    ASSERT_TRUE(AliasMapReplace(map_ptr, group, 2, "v2"));
    ExpectValue(map_ptr, "a", "v2");
    ExpectValue(map_ptr, "b", "v2");
    EXPECT_EQ(Group(map_ptr, "a"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 2u);

    ASSERT_TRUE(AliasMapReplace(map_ptr, group, 1, "v3"));
    ExpectValue(map_ptr, "a", "v3");
    ExpectValue(map_ptr, "b", "v2");
}

TEST_F(AliasMapTest, ReplaceOnRemovedAliasIsAFreshInsert)
{
    const char *group[] = { "a" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "v1"));
    ASSERT_TRUE(AliasMapRemove(map_ptr, "a"));

    ASSERT_TRUE(AliasMapReplace(map_ptr, group, 1, "v2"));
    ExpectValue(map_ptr, "a", "v2");
}

/* ── Growth ────────────────────────────────────────────────────────────── */

TEST_F(AliasMapTest, GrowsAcrossThousandsOfGroups)
{
    const int groups = 2000;

    for (int i = 0; i < groups; i++) {
        std::string first  = "a" + std::to_string(i);
        std::string second = "b" + std::to_string(i);
        std::string value  = Value(i);
        const char *pair[] = { first.c_str(), second.c_str() };

        ASSERT_TRUE(AliasMapAdd(map_ptr, pair, 2, value.c_str()));
    }

    EXPECT_EQ(AliasMapAliasCount(map_ptr), (size_t)groups * 2);

    for (int i = 0; i < groups; i++) {
        std::string first  = "a" + std::to_string(i);
        std::string second = "b" + std::to_string(i);

        ExpectValue(map_ptr, first.c_str(), Value(i).c_str());
        ExpectValue(map_ptr, second.c_str(), Value(i).c_str());
    }

    for (int i = 0; i < groups; i += 2) {
        std::string first = "a" + std::to_string(i);

        EXPECT_EQ(AliasMapRemoveValue(map_ptr, first.c_str()), 2u);
    }

    EXPECT_EQ(AliasMapAliasCount(map_ptr), (size_t)groups);
}

/* ── Concurrency ───────────────────────────────────────────────────────── */

TEST_F(AliasMapTest, ConcurrentDisjointKeys)
{
    const int thread_count = 8;
    const int per_thread   = 500;

    std::vector<std::thread> threads;

    threads.reserve(thread_count);

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([this, t, per_thread] {
            for (int i = 0; i < per_thread; i++) {
                int         index  = t * per_thread + i;
                std::string key    = Key(index);
                std::string alias  = "a" + std::to_string(index);
                std::string value  = Value(index);
                const char *group[] = { key.c_str(), alias.c_str() };

                ASSERT_TRUE(AliasMapAdd(map_ptr, group, 2, value.c_str()));
            }
        });
    }

    for (auto &thread : threads) thread.join();

    EXPECT_EQ(AliasMapAliasCount(map_ptr), (size_t)thread_count * per_thread * 2);

    for (int i = 0; i < thread_count * per_thread; i++) {
        std::string key   = Key(i);
        std::string alias = "a" + std::to_string(i);
        std::string value = Value(i);

        ExpectValue(map_ptr, key.c_str(), value.c_str());
        ExpectValue(map_ptr, alias.c_str(), value.c_str());
    }
}

TEST_F(AliasMapTest, ConcurrentSameKeysOnlyOneWriterWins)
{
    const int thread_count = 8;
    const int key_count    = 256;

    std::atomic<int> successes{0};
    std::vector<std::thread> threads;

    threads.reserve(thread_count);

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([this, key_count, &successes] {
            for (int i = 0; i < key_count; i++) {
                std::string key    = Key(i);
                std::string value  = Value(i);
                const char *group[] = { key.c_str() };

                if (AliasMapAdd(map_ptr, group, 1, value.c_str())) successes++;
            }
        });
    }

    for (auto &thread : threads) thread.join();

    EXPECT_EQ(successes.load(), key_count);
    EXPECT_EQ(AliasMapAliasCount(map_ptr), (size_t)key_count);

    for (int i = 0; i < key_count; i++)
        ExpectValue(map_ptr, Key(i).c_str(), Value(i).c_str());
}

TEST_F(AliasMapTest, ConcurrentMixedWorkload)
{
    const int thread_count = 8;
    const int key_count    = 64;
    const int iterations   = 20000;

    std::vector<std::thread> threads;

    threads.reserve(thread_count);

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([this, t, key_count, iterations] {
            unsigned seed = 1234U + (unsigned)t;

            for (int i = 0; i < iterations; i++) {
                std::string key    = Key((int)(rand_r(&seed) % key_count));
                std::string alias  = Key((int)(rand_r(&seed) % key_count));
                std::string value  = Value((int)(rand_r(&seed) % 1000));

                switch (rand_r(&seed) % 8) {
                case 0: {
                    const char *group[] = { key.c_str(), alias.c_str() };
                    AliasMapAdd(map_ptr, group, 2, value.c_str());
                    break;
                }
                case 1:
                    AliasMapAddAlias(map_ptr, key.c_str(), alias.c_str());
                    break;
                case 2:
                    AliasMapSetValue(map_ptr, key.c_str(), value.c_str());
                    break;
                case 3:
                    AliasMapRemove(map_ptr, key.c_str());
                    break;
                case 4:
                    AliasMapRemoveValue(map_ptr, key.c_str());
                    break;
                case 5:
                    AliasMapContains(map_ptr, key.c_str());
                    break;
                case 6: {
                    const char *group[] = { key.c_str(), alias.c_str() };
                    AliasMapReplace(map_ptr, group, 2, value.c_str());
                    break;
                }
                default: {
                    size_t count;
                    free(AliasMapAliasesOf(map_ptr, key.c_str(), &count));
                    break;
                }
                }
            }
        });
    }

    for (auto &thread : threads) thread.join();

    CheckInvariants(map_ptr, key_count);
}

TEST_F(AliasMapTest, ConcurrentInsertRemove)
{
    const int thread_count = 8;
    const int key_count    = 32;
    const int iterations   = 10000;

    std::vector<std::thread> threads;

    threads.reserve(thread_count);

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([this, t, key_count, iterations] {
            unsigned seed = 99U + (unsigned)t;

            for (int i = 0; i < iterations; i++) {
                std::string key    = Key((int)(rand_r(&seed) % key_count));
                std::string alias  = Key((int)(rand_r(&seed) % key_count));
                std::string value  = Value((int)(rand_r(&seed) % 100));
                const char *group[] = { key.c_str(), alias.c_str() };

                if (rand_r(&seed) & 1U) {
                    AliasMapAdd(map_ptr, group, 2, value.c_str());
                }
                else {
                    AliasMapRemoveValue(map_ptr, key.c_str());
                }
            }
        });
    }

    for (auto &thread : threads) thread.join();

    CheckInvariants(map_ptr, key_count);
}

TEST_F(AliasMapTest, ConcurrentHighContention)
{
    const int thread_count = 12;
    const int key_count    = 16;
    const int iterations   = 20000;

    std::vector<std::thread> threads;

    threads.reserve(thread_count);

    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([this, t, key_count, iterations] {
            unsigned seed = 7U + (unsigned)t;

            for (int i = 0; i < iterations; i++) {
                std::string key    = Key((int)(rand_r(&seed) % key_count));
                std::string alias  = Key((int)(rand_r(&seed) % key_count));
                std::string value  = Value((int)(rand_r(&seed) % 50));

                if (rand_r(&seed) % 3 == 0) {
                    const char *group[] = { key.c_str(), alias.c_str() };
                    AliasMapReplace(map_ptr, group, 2, value.c_str());
                }
                else if (rand_r(&seed) & 1U) {
                    const char *group[] = { key.c_str() };
                    AliasMapAdd(map_ptr, group, 1, value.c_str());
                }
                else {
                    AliasMapRemoveValue(map_ptr, key.c_str());
                }
            }
        });
    }

    for (auto &thread : threads) thread.join();

    CheckInvariants(map_ptr, key_count);
}

TEST_F(AliasMapTest, ConcurrentReplaceIsAtomicallyVisible)
{
    static const char *const set_a[] = { "k0", "k1", "k2" };
    static const char *const set_b[] = { "k2", "k3", "k4" };

    const int iterations = 20000;

    std::atomic<int> failures{0};

    auto writes = [this, iterations](const char *const *set_ptr, int id, std::atomic<int> *failures_ptr) {
        for (int i = 0; i < iterations; i++) {
            std::string value = "w" + std::to_string(id) + "-" + std::to_string(i);

            if (!AliasMapReplace(map_ptr, set_ptr, 3, value.c_str()))
                failures_ptr->fetch_add(1);
        }
    };

    auto watches = [this, iterations, &failures](const char *alias_ptr,
                                                 const std::vector<std::string> &a,
                                                 const std::vector<std::string> &b) {
        for (int i = 0; i < iterations; i++) {
            auto group = Group(map_ptr, alias_ptr);

            if (!group.empty() && group != a && group != b) failures.fetch_add(1);
        }
    };

    const std::vector<std::string> group_a = {"k0", "k1", "k2"};
    const std::vector<std::string> group_b = {"k2", "k3", "k4"};
    const std::vector<std::string> part_a  = {"k0", "k1"};
    const std::vector<std::string> part_b  = {"k3", "k4"};

    std::vector<std::thread> threads;

    threads.emplace_back(writes, set_a, 1, &failures);
    threads.emplace_back(writes, set_b, 2, &failures);
    threads.emplace_back(watches, "k0", group_a, part_a);
    threads.emplace_back(watches, "k1", group_a, part_a);
    threads.emplace_back(watches, "k2", group_a, group_b);
    threads.emplace_back(watches, "k3", group_b, part_b);
    threads.emplace_back(watches, "k4", group_b, part_b);

    for (auto &thread : threads) thread.join();

    EXPECT_EQ(failures.load(), 0);
    CheckInvariants(map_ptr, 5);
}


/* ── Descriptor construction ───────────────────────────────────────────── */

class AliasMapDescriptorTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        AliasMapDestroy(map_ptr);
        map_ptr = nullptr;
    }

    AliasMap *Build()
    {
        map_ptr = AliasMapCreateWithDescriptor(&descriptor);

        return map_ptr;
    }

    AliasMapDescriptor descriptor = {};
    AliasMap          *map_ptr    = nullptr;
};

TEST_F(AliasMapDescriptorTest, LoadsCollectorsInBulk)
{
    static const char *const greeting[] = { "greeting", "hello" };
    static const char *const motd[]     = { "motd" };

    static const AliasMapCollector collectors[] = {
        { "/opt/etc/ufsrv/assets/greeting.txt", greeting, 2 },
        { "/opt/etc/ufsrv/assets/motd.txt",     motd,     1 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 2;

    ASSERT_NE(Build(), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_OK);
    EXPECT_EQ(descriptor.result.group_count, 2u);
    EXPECT_EQ(descriptor.result.alias_count, 3u);

    EXPECT_EQ(AliasMapAliasCount(map_ptr), 3u);
    ExpectValue(map_ptr, "greeting", "/opt/etc/ufsrv/assets/greeting.txt");
    ExpectValue(map_ptr, "hello", "/opt/etc/ufsrv/assets/greeting.txt");
    ExpectValue(map_ptr, "motd", "/opt/etc/ufsrv/assets/motd.txt");
}

TEST_F(AliasMapDescriptorTest, NullDescriptorBuildsEmptyMap)
{
    map_ptr = AliasMapCreateWithDescriptor(nullptr);

    ASSERT_NE(map_ptr, nullptr);
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 0u);
}

TEST_F(AliasMapDescriptorTest, ZeroCollectorsBuildsEmptyMap)
{
    descriptor.collectors_ptr  = nullptr;
    descriptor.collector_count = 0;

    ASSERT_NE(Build(), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_OK);
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 0u);
}

TEST_F(AliasMapDescriptorTest, RejectsMissingCollectorArray)
{
    descriptor.collectors_ptr  = nullptr;
    descriptor.collector_count = 1;

    EXPECT_EQ(Build(), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
}

TEST_F(AliasMapDescriptorTest, RejectsCollectorWithoutValue)
{
    static const char *const aliases[] = { "a" };

    static const AliasMapCollector collectors[] = {
        { nullptr, aliases, 1 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 1;

    EXPECT_EQ(Build(), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
    EXPECT_EQ(descriptor.result.failed_collector, 0u);
    EXPECT_EQ(descriptor.result.failed_alias, SIZE_MAX);
}

TEST_F(AliasMapDescriptorTest, RejectsCollectorWithoutAliases)
{
    static const AliasMapCollector collectors[] = {
        { "value", nullptr, 1 },
        { "value", nullptr, 0 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 1;

    EXPECT_EQ(Build(), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);

    descriptor.collector_count = 2;

    EXPECT_EQ(Build(), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
    EXPECT_EQ(descriptor.result.failed_collector, 0u);
}

TEST_F(AliasMapDescriptorTest, RejectsNullAliasElement)
{
    static const char *const aliases[] = { "a", nullptr };
    static const AliasMapCollector collectors[] = {
        { "value", aliases, 2 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 1;

    EXPECT_EQ(Build(), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
    EXPECT_EQ(descriptor.result.failed_collector, 0u);
    EXPECT_EQ(descriptor.result.failed_alias, 1u);
}

TEST_F(AliasMapDescriptorTest, RejectsAliasCountOverflow)
{
    static const char *const aliases[] = { "a" };
    static const AliasMapCollector collectors[] = {
        { "value", aliases, SIZE_MAX - 1 },
        { "value", aliases, 2 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 2;

    EXPECT_EQ(Build(), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
    EXPECT_EQ(descriptor.result.failed_collector, 1u);
}

TEST_F(AliasMapDescriptorTest, RejectsDuplicateAliasAndReportsBothOccurrences)
{
    static const char *const first[]  = { "dup", "x" };
    static const char *const second[] = { "y", "dup" };

    static const AliasMapCollector collectors[] = {
        { "v1", first,  2 },
        { "v2", second, 2 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 2;

    EXPECT_EQ(Build(), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_DUPLICATE_ALIAS);
    EXPECT_EQ(descriptor.result.failed_collector, 1u);
    EXPECT_EQ(descriptor.result.failed_alias, 1u);
    EXPECT_EQ(descriptor.result.conflict_collector, 0u);
    EXPECT_EQ(descriptor.result.conflict_alias, 0u);
    EXPECT_EQ(descriptor.result.group_count, 0u);
}

TEST_F(AliasMapDescriptorTest, DescriptorMapIsFullyOperational)
{
    static const char *const group[] = { "greeting", "hello" };
    static const AliasMapCollector collectors[] = {
        { "/opt/etc/ufsrv/assets/greeting.txt", group, 2 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 1;

    ASSERT_NE(Build(), nullptr);

    const char *extra[] = { "greeting", "howdy" };

    ASSERT_TRUE(AliasMapReplace(map_ptr, extra, 2, "/opt/etc/ufsrv/assets/hi.txt"));
    ExpectValue(map_ptr, "howdy", "/opt/etc/ufsrv/assets/hi.txt");
    ExpectValue(map_ptr, "hello", "/opt/etc/ufsrv/assets/greeting.txt");

    ASSERT_TRUE(AliasMapAddAlias(map_ptr, "howdy", "heya"));
    ASSERT_TRUE(AliasMapSetValue(map_ptr, "howdy", "/opt/etc/ufsrv/assets/renamed.txt"));
    ExpectValue(map_ptr, "heya", "/opt/etc/ufsrv/assets/renamed.txt");

    ASSERT_TRUE(AliasMapRemove(map_ptr, "heya"));
    ExpectAbsent(map_ptr, "heya");

    EXPECT_EQ(AliasMapRemoveValue(map_ptr, "hello"), 1u);
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 2u);
}

TEST_F(AliasMapDescriptorTest, MatchesIncrementalConstruction)
{
    static const char *const g1[] = { "a", "b" };
    static const char *const g2[] = { "c" };
    static const AliasMapCollector collectors[] = {
        { "/p/one", g1, 2 },
        { "/p/two", g2, 1 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 2;

    ASSERT_NE(Build(), nullptr);

    AliasMap *incremental_ptr = AliasMapCreate();

    ASSERT_NE(incremental_ptr, nullptr);
    ASSERT_TRUE(AliasMapAdd(incremental_ptr, g1, 2, "/p/one"));
    ASSERT_TRUE(AliasMapAdd(incremental_ptr, g2, 1, "/p/two"));

    EXPECT_EQ(AliasMapAliasCount(map_ptr), AliasMapAliasCount(incremental_ptr));

    for (const char *alias : { "a", "b", "c" }) {
        char   one[64], two[64];
        size_t len_one = 0, len_two = 0;

        ASSERT_TRUE(AliasMapGetInto(map_ptr, alias, one, sizeof one, &len_one));
        ASSERT_TRUE(AliasMapGetInto(incremental_ptr, alias, two, sizeof two, &len_two));
        EXPECT_EQ(std::string(one), std::string(two));
        EXPECT_EQ(Group(map_ptr, alias), Group(incremental_ptr, alias));
    }

    AliasMapDestroy(incremental_ptr);
}

/* ── Describe ──────────────────────────────────────────────────────────── */

TEST_F(AliasMapTest, DescribeReportsMetadata)
{
    const char *first[]  = { "a", "b" };
    const char *second[] = { "c" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, first, 2, "one"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, second, 1, "two"));

    BufferDescriptor *description = AliasMapDescribe(map_ptr, nullptr);

    ASSERT_NE(description, nullptr);

    const char *json = description->data;

    EXPECT_NE(strstr(json, "\"groups\":2"), nullptr);
    EXPECT_NE(strstr(json, "\"aliases\":3"), nullptr);
    EXPECT_NE(strstr(json, "\"groups_min\":1"), nullptr);
    EXPECT_NE(strstr(json, "\"groups_max\":2"), nullptr);
    EXPECT_NE(strstr(json, "\"groups_mean\":1.50"), nullptr);
    EXPECT_NE(strstr(json, "\"hash\":\"fnv1a-64\""), nullptr);
    EXPECT_NE(strstr(json, "\"logger\":\"none\""), nullptr);
    EXPECT_NE(strstr(json, "\"truncated\":false"), nullptr);
    EXPECT_NE(strstr(json, "\"groups_detail\":["), nullptr);
    EXPECT_NE(strstr(json, "\"value\":\"one\""), nullptr);
    EXPECT_NE(strstr(json, "\"value\":\"two\""), nullptr);

    BufferDescriptorRelease(description);
    free(description);
}

TEST_F(AliasMapTest, DescribeAppendsToProvidedBuffer)
{
    const char *group[] = { "a" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "one"));

    BufferDescriptor buffer;

    BufferDescriptorInit(&buffer, 64);
    BufferDescriptorAppendFormatted(&buffer, "prefix ");

    BufferDescriptor *description = AliasMapDescribe(map_ptr, &buffer);

    ASSERT_EQ(description, &buffer);
    EXPECT_EQ(strncmp(buffer.data, "prefix {", 8), 0);
    EXPECT_NE(strstr(buffer.data, "\"aliases\":1"), nullptr);

    BufferDescriptorRelease(&buffer);
}

TEST(AliasMapDescribeTest, ReportsNullHandle)
{
    BufferDescriptor *description = AliasMapDescribe(nullptr, nullptr);

    ASSERT_NE(description, nullptr);
    EXPECT_NE(strstr(description->data, "\"error\":\"null handle\""), nullptr);

    BufferDescriptorRelease(description);
    free(description);
}

TEST_F(AliasMapTest, DescribeEscapesStrings)
{
    const char *group[] = { "esc" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "a\"b\nc"));

    BufferDescriptor *description = AliasMapDescribe(map_ptr, nullptr);

    ASSERT_NE(description, nullptr);
    EXPECT_NE(strstr(description->data, "a\\\"b\\nc"), nullptr);

    BufferDescriptorRelease(description);
    free(description);
}

TEST_F(AliasMapTest, DescribeTruncatesAtCap)
{
    for (int i = 0; i < 200; i++) {
        std::string first  = "k" + std::to_string(i) + "a";
        std::string second = "k" + std::to_string(i) + "b";
        std::string value  = "v" + std::to_string(i);
        const char *group[] = { first.c_str(), second.c_str() };

        ASSERT_TRUE(AliasMapAdd(map_ptr, group, 2, value.c_str()));
    }

    BufferDescriptor *description = AliasMapDescribe(map_ptr, nullptr);

    ASSERT_NE(description, nullptr);
    EXPECT_NE(strstr(description->data, "\"truncated\":true"), nullptr);
    EXPECT_NE(strstr(description->data, "\"groups\":200"), nullptr);

    BufferDescriptorRelease(description);
    free(description);
}

TEST_F(AliasMapDescriptorTest, DescribeReportsLoggerEnabled)
{
    UfLogger *log_ptr = nullptr;

    if (UfLoggerCreateWithDefaults(&log_ptr) != UF_LOGGER_STATUS_OK || !log_ptr)
        GTEST_SKIP() << "no logger backend bound";

    static const char *const group[] = { "logged" };
    static const AliasMapCollector collectors[] = {
        { "/p/logged", group, 1 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 1;
    descriptor.logger_ptr      = log_ptr;

    ASSERT_NE(Build(), nullptr);

    BufferDescriptor *description = AliasMapDescribe(map_ptr, nullptr);

    ASSERT_NE(description, nullptr);
    EXPECT_NE(strstr(description->data, "\"logger\":\"enabled\""), nullptr);

    BufferDescriptorRelease(description);
    free(description);

    AliasMapDestroy(map_ptr);
    map_ptr = nullptr;

    UfLoggerDestroy(log_ptr);
}


/* ── JSON descriptor construction ─────────────────────────────────────── */

/* ── Snapshot ──────────────────────────────────────────────────────────── */

// Groups rendered as "value{alias,alias}" in map order.
std::vector<std::string>
SnapshotGroups(const AliasMapSnapshot *snapshot_ptr)
{
    std::vector<std::string> out;

    for (size_t g = 0; g < AliasMapSnapshotGroupCount(snapshot_ptr); g++) {
        std::string group = AliasMapSnapshotValueAt(snapshot_ptr, g);

        group += "{";

        for (size_t a = 0; a < AliasMapSnapshotAliasCountAt(snapshot_ptr, g); a++) {
            if (a) group += ",";
            group += AliasMapSnapshotAliasAt(snapshot_ptr, g, a);
        }

        out.push_back(group + "}");
    }

    return out;
}

TEST_F(AliasMapTest, SnapshotOfEmptyMap)
{
    AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);

    ASSERT_NE(snapshot_ptr, nullptr);
    EXPECT_EQ(AliasMapSnapshotGroupCount(snapshot_ptr), 0u);
    EXPECT_EQ(AliasMapSnapshotAliasCount(snapshot_ptr), 0u);
    EXPECT_EQ(AliasMapSnapshotValueAt(snapshot_ptr, 0), nullptr);
    EXPECT_EQ(AliasMapSnapshotAliasCountAt(snapshot_ptr, 0), 0u);
    EXPECT_EQ(AliasMapSnapshotAliasAt(snapshot_ptr, 0, 0), nullptr);

    AliasMapSnapshotDestroy(snapshot_ptr);
}

TEST_F(AliasMapTest, SnapshotGroupsFollowCreationOrder)
{
    const char *one[]   = { "a", "b" };
    const char *two[]   = { "c", "d", "e" };
    const char *three[] = { "f" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, one, 2, "v1"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, two, 3, "v2"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, three, 1, "v3"));

    AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);

    ASSERT_NE(snapshot_ptr, nullptr);
    EXPECT_EQ(AliasMapSnapshotGroupCount(snapshot_ptr), 3u);
    EXPECT_EQ(AliasMapSnapshotAliasCount(snapshot_ptr), 6u);

    // Group order is insertion order, alias order is most recently attached first.
    EXPECT_EQ(SnapshotGroups(snapshot_ptr),
              (std::vector<std::string>{"v1{b,a}", "v2{e,d,c}", "v3{f}"}));

    AliasMapSnapshotDestroy(snapshot_ptr);
}

TEST_F(AliasMapTest, SnapshotReflectsRemovalAndReplace)
{
    const char *one[]   = { "a", "b" };
    const char *two[]   = { "c" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, one, 2, "v1"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, two, 1, "v2"));

    const char *three[] = { "d" };
    ASSERT_TRUE(AliasMapAdd(map_ptr, three, 1, "v3"));

    EXPECT_EQ(AliasMapRemoveValue(map_ptr, "a"), 2u);   /* v1 gone */

    const char *stolen[] = { "c" };

    ASSERT_TRUE(AliasMapReplace(map_ptr, stolen, 1, "v9")); /* v2 emptied, new group appended */

    AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);

    ASSERT_NE(snapshot_ptr, nullptr);
    EXPECT_EQ(SnapshotGroups(snapshot_ptr),
              (std::vector<std::string>{"v3{d}", "v9{c}"}));

    AliasMapSnapshotDestroy(snapshot_ptr);
}

TEST_F(AliasMapTest, SnapshotIsIndependentOfLaterMutation)
{
    const char *group[] = { "ll", "lsl" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 2, "ls -la"));

    AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);

    ASSERT_NE(snapshot_ptr, nullptr);
    EXPECT_EQ(AliasMapRemoveValue(map_ptr, "ll"), 2u);

    const char *other[] = { "dirlist" };
    ASSERT_TRUE(AliasMapAdd(map_ptr, other, 1, "ls -ld"));

    AliasMapSnapshot *second_ptr = AliasMapSnapshotCreate(map_ptr);

    ASSERT_NE(second_ptr, nullptr);

    /* Each snapshot keeps the state it captured. */
    EXPECT_EQ(SnapshotGroups(snapshot_ptr), (std::vector<std::string>{"ls -la{lsl,ll}"}));
    EXPECT_EQ(SnapshotGroups(second_ptr), (std::vector<std::string>{"ls -ld{dirlist}"}));

    /* A snapshot outlives the map. */
    AliasMapDestroy(map_ptr);
    map_ptr = nullptr;

    EXPECT_EQ(SnapshotGroups(snapshot_ptr), (std::vector<std::string>{"ls -la{lsl,ll}"}));

    AliasMapSnapshotDestroy(snapshot_ptr);
    AliasMapSnapshotDestroy(second_ptr);
}

TEST_F(AliasMapTest, SnapshotRejectsInvalidArguments)
{
    EXPECT_EQ(AliasMapSnapshotCreate(nullptr), nullptr);
    EXPECT_EQ(AliasMapSnapshotGroupCount(nullptr), 0u);
    EXPECT_EQ(AliasMapSnapshotAliasCount(nullptr), 0u);
    EXPECT_EQ(AliasMapSnapshotValueAt(nullptr, 0), nullptr);
    EXPECT_EQ(AliasMapSnapshotAliasCountAt(nullptr, 0), 0u);
    EXPECT_EQ(AliasMapSnapshotAliasAt(nullptr, 0, 0), nullptr);

    AliasMapSnapshotDestroy(nullptr);

    const char *group[] = { "ll" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "v"));

    AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);

    ASSERT_NE(snapshot_ptr, nullptr);
    EXPECT_EQ(AliasMapSnapshotValueAt(snapshot_ptr, 1), nullptr);
    EXPECT_EQ(AliasMapSnapshotAliasCountAt(snapshot_ptr, 1), 0u);
    EXPECT_EQ(AliasMapSnapshotAliasAt(snapshot_ptr, 0, 1), nullptr);

    AliasMapSnapshotDestroy(snapshot_ptr);
}

/* ── ForEach ───────────────────────────────────────────────────────────── */

struct VisitLog {
    std::vector<std::string> items;
    size_t                   stop_after = SIZE_MAX;
};

bool
RecordVisit(void *context_ptr, const char *value_ptr, const char *alias_ptr)
{
    VisitLog *log = static_cast<VisitLog *>(context_ptr);

    log->items.emplace_back(std::string(alias_ptr) + "->" + value_ptr);

    return log->items.size() < log->stop_after;
}

TEST_F(AliasMapTest, ForEachVisitsEveryAliasInOrder)
{
    const char *one[]   = { "a", "b" };
    const char *two[]   = { "c" };
    const char *three[] = { "d", "e" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, one, 2, "v1"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, two, 1, "v2"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, three, 2, "v3"));

    VisitLog log;
    EXPECT_TRUE(AliasMapForEach(map_ptr, RecordVisit, &log));

    EXPECT_EQ(log.items, (std::vector<std::string>{"b->v1", "a->v1", "c->v2", "e->v3", "d->v3"}));
}

TEST_F(AliasMapTest, ForEachStopsWhenVisitorReturnsFalse)
{
    const char *group[] = { "a", "b", "c" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 3, "v1"));

    VisitLog log;
    log.stop_after = 2;

    EXPECT_FALSE(AliasMapForEach(map_ptr, RecordVisit, &log));
    EXPECT_EQ(log.items.size(), 2u);
}

TEST_F(AliasMapTest, ForEachOnEmptyMapCompletes)
{
    VisitLog log;

    EXPECT_TRUE(AliasMapForEach(map_ptr, RecordVisit, &log));
    EXPECT_TRUE(log.items.empty());
}

TEST_F(AliasMapTest, ForEachRejectsInvalidArguments)
{
    VisitLog log;

    EXPECT_FALSE(AliasMapForEach(nullptr, RecordVisit, &log));
    EXPECT_TRUE(log.items.empty());

    EXPECT_FALSE(AliasMapForEach(map_ptr, nullptr, &log));
    EXPECT_TRUE(log.items.empty());
}

TEST_F(AliasMapTest, ForEachMatchesSnapshot)
{
    const char *one[] = { "a", "b" };
    const char *two[] = { "c" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, one, 2, "v1"));
    ASSERT_TRUE(AliasMapAdd(map_ptr, two, 1, "v2"));

    VisitLog        log;
    AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);

    ASSERT_NE(snapshot_ptr, nullptr);
    ASSERT_TRUE(AliasMapForEach(map_ptr, RecordVisit, &log));

    std::vector<std::string> from_snapshot;

    for (size_t g = 0; g < AliasMapSnapshotGroupCount(snapshot_ptr); g++)
        for (size_t a = 0; a < AliasMapSnapshotAliasCountAt(snapshot_ptr, g); a++)
            from_snapshot.emplace_back(std::string(AliasMapSnapshotAliasAt(snapshot_ptr, g, a)) +
                                       "->" + AliasMapSnapshotValueAt(snapshot_ptr, g));

    EXPECT_EQ(log.items, from_snapshot);

    AliasMapSnapshotDestroy(snapshot_ptr);
}

class AliasMapJsonTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        AliasMapDestroy(map_ptr);
        map_ptr = nullptr;
    }

    AliasMap *Build(const char *json_ptr)
    {
        descriptor.json_ptr = json_ptr;
        map_ptr = AliasMapCreateWithJsonDescriptor(&descriptor);

        return map_ptr;
    }

    AliasMapJsonDescriptor descriptor = {};
    AliasMap               *map_ptr    = nullptr;
};

TEST_F(AliasMapJsonTest, ParsesArrayOfGroups)
{
    const char *json = R"json([
        {"value": "/opt/etc/ufsrv/assets/greeting.txt", "keys": ["greeting", "hello"]},
        {"value": "/opt/etc/ufsrv/assets/motd.txt",     "keys": ["motd"]}
    ])json";

    ASSERT_NE(Build(json), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_OK);
    EXPECT_EQ(descriptor.result.group_count, 2u);
    EXPECT_EQ(descriptor.result.alias_count, 3u);

    ExpectValue(map_ptr, "greeting", "/opt/etc/ufsrv/assets/greeting.txt");
    ExpectValue(map_ptr, "hello", "/opt/etc/ufsrv/assets/greeting.txt");
    ExpectValue(map_ptr, "motd", "/opt/etc/ufsrv/assets/motd.txt");
}

TEST_F(AliasMapJsonTest, EmptyArrayBuildsEmptyMap)
{
    ASSERT_NE(Build("  [ ]  "), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_OK);
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 0u);
}

TEST_F(AliasMapJsonTest, DecodesEscapesAndUnicode)
{
    const char *json = R"json([{"value": "a\"b\\c\n\u00e9\uD83D\uDE00", "keys": ["escaped"]}])json";

    ASSERT_NE(Build(json), nullptr);

    char   value[64];
    size_t length = 0;

    ASSERT_TRUE(AliasMapGetInto(map_ptr, "escaped", value, sizeof value, &length));
    EXPECT_EQ(std::string(value), std::string("a\"b\\c\n\xC3\xA9\xF0\x9F\x98\x80"));
}

TEST_F(AliasMapJsonTest, NullDescriptorIsRejected)
{
    EXPECT_EQ(AliasMapCreateWithJsonDescriptor(nullptr), nullptr);
}

TEST_F(AliasMapJsonTest, NullJsonIsRejected)
{
    EXPECT_EQ(Build(nullptr), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
    EXPECT_EQ(descriptor.result.json_offset, 0u);
}

TEST_F(AliasMapJsonTest, RejectsNonArrayDocument)
{
    EXPECT_EQ(Build(R"json({"value": "v", "keys": ["a"]})json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
    EXPECT_EQ(descriptor.result.json_offset, 0u);
}

TEST_F(AliasMapJsonTest, RejectsEmptyObject)
{
    EXPECT_EQ(Build("[{}]"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
}

TEST_F(AliasMapJsonTest, RejectsMissingValue)
{
    EXPECT_EQ(Build(R"json([{"keys": ["a"]}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
}

TEST_F(AliasMapJsonTest, RejectsMissingKeys)
{
    EXPECT_EQ(Build(R"json([{"value": "v"}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
}

TEST_F(AliasMapJsonTest, RejectsUnknownField)
{
    EXPECT_EQ(Build(R"json([{"value": "v", "keys": ["a"], "extra": 1}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
}

TEST_F(AliasMapJsonTest, RejectsRepeatedField)
{
    EXPECT_EQ(Build(R"json([{"value": "v", "value": "w", "keys": ["a"]}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
}

TEST_F(AliasMapJsonTest, RejectsEmptyKeys)
{
    EXPECT_EQ(Build(R"json([{"value": "v", "keys": []}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
}

TEST_F(AliasMapJsonTest, RejectsKeysWrongType)
{
    EXPECT_EQ(Build(R"json([{"value": "v", "keys": "a"}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
}

TEST_F(AliasMapJsonTest, RejectsValueWrongType)
{
    EXPECT_EQ(Build(R"json([{"value": 7, "keys": ["a"]}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_ARGUMENT);
}

TEST_F(AliasMapJsonTest, RejectsMalformedJson)
{
    EXPECT_EQ(Build(R"json([{"value": "v", "keys": ["a"}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_JSON);

    EXPECT_EQ(Build(R"json([{"value": "v", "keys": ["a\q"]}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_JSON);

    EXPECT_EQ(Build(R"json([{"value": "v", "keys": ["\u12"]}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_JSON);

    EXPECT_EQ(Build(R"json([{"value": "v", "keys": ["\uD83D"]}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_JSON);

    EXPECT_EQ(Build(R"json([{"value": "unterminated, "keys": ["a"]}])json"), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_JSON);
}

TEST_F(AliasMapJsonTest, RejectsTrailingData)
{
    const char *json = R"json([{"value": "v", "keys": ["a"]}] x)json";

    EXPECT_EQ(Build(json), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_INVALID_JSON);
    EXPECT_EQ(descriptor.result.json_offset, strlen(json) - 1);
}

TEST_F(AliasMapJsonTest, RejectsDuplicateAliasWithCoordinates)
{
    const char *json = R"json([
        {"value": "v1", "keys": ["dup", "x"]},
        {"value": "v2", "keys": ["y", "dup"]}
    ])json";

    EXPECT_EQ(Build(json), nullptr);
    EXPECT_EQ(descriptor.result.status, ALIASMAP_CREATE_DUPLICATE_ALIAS);
    EXPECT_EQ(descriptor.result.failed_collector, 1u);
    EXPECT_EQ(descriptor.result.failed_alias, 1u);
    EXPECT_EQ(descriptor.result.conflict_collector, 0u);
    EXPECT_EQ(descriptor.result.conflict_alias, 0u);
}

TEST_F(AliasMapJsonTest, JsonMapIsFullyOperational)
{
    ASSERT_NE(Build(R"json([{"value": "/p/one", "keys": ["one", "uno"]}])json"), nullptr);

    ASSERT_TRUE(AliasMapAddAlias(map_ptr, "one", "eins"));
    ExpectValue(map_ptr, "eins", "/p/one");

    const char *group[] = { "one" };
    ASSERT_TRUE(AliasMapReplace(map_ptr, group, 1, "/p/two"));
    ExpectValue(map_ptr, "one", "/p/two");
    ExpectValue(map_ptr, "uno", "/p/one");

    /* "one" was moved out by the replace, so the old group holds uno + eins. */
    EXPECT_EQ(AliasMapRemoveValue(map_ptr, "uno"), 2u);
    ASSERT_TRUE(AliasMapRemove(map_ptr, "one"));
    ExpectAbsent(map_ptr, "one");
}

TEST_F(AliasMapJsonTest, JsonMatchesStructConstruction)
{
    static const char *const greeting[] = { "greeting", "hello" };
    static const AliasMapCollector collectors[] = {
        { "/p/greeting", greeting, 2 },
    };

    AliasMapDescriptor struct_descriptor = {};

    struct_descriptor.collectors_ptr  = collectors;
    struct_descriptor.collector_count = 1;

    AliasMap *struct_map = AliasMapCreateWithDescriptor(&struct_descriptor);

    ASSERT_NE(struct_map, nullptr);

    ASSERT_NE(Build(R"json([{"value": "/p/greeting", "keys": ["greeting", "hello"]}])json"),
              nullptr);

    EXPECT_EQ(AliasMapAliasCount(map_ptr), AliasMapAliasCount(struct_map));
    EXPECT_EQ(Group(map_ptr, "greeting"), Group(struct_map, "greeting"));
    ExpectValue(map_ptr, "hello", "/p/greeting");

    AliasMapDestroy(struct_map);
}


/* ── Logger-bound construction ────────────────────────────────────────── */

TEST(AliasMapCreateWithLoggerTest, NullLoggerIsSilent)
{
    AliasMap *map_ptr = AliasMapCreateWithLogger(nullptr);

    ASSERT_NE(map_ptr, nullptr);
    EXPECT_EQ(AliasMapAliasCount(map_ptr), 0u);

    BufferDescriptor *description = AliasMapDescribe(map_ptr, nullptr);

    ASSERT_NE(description, nullptr);
    EXPECT_NE(strstr(description->data, "\"logger\":\"none\""), nullptr);

    BufferDescriptorRelease(description);
    free(description);

    AliasMapDestroy(map_ptr);
}

TEST(AliasMapCreateWithLoggerTest, BindsLoggerAtConstruction)
{
    UfLogger *log_ptr = nullptr;

    if (UfLoggerCreateWithDefaults(&log_ptr) != UF_LOGGER_STATUS_OK || !log_ptr)
        GTEST_SKIP() << "no logger backend bound";

    AliasMap *map_ptr = AliasMapCreateWithLogger(log_ptr);

    ASSERT_NE(map_ptr, nullptr);

    const char *group[] = { "logged" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 1, "/p/logged"));
    ExpectValue(map_ptr, "logged", "/p/logged");

    BufferDescriptor *description = AliasMapDescribe(map_ptr, nullptr);

    ASSERT_NE(description, nullptr);
    EXPECT_NE(strstr(description->data, "\"logger\":\"enabled\""), nullptr);

    BufferDescriptorRelease(description);
    free(description);

    /* A rejected duplicate travels through the bound multiplexer. */
    EXPECT_FALSE(AliasMapAdd(map_ptr, group, 1, "/p/other"));
    ExpectValue(map_ptr, "logged", "/p/logged");

    AliasMapDestroy(map_ptr);
    UfLoggerDestroy(log_ptr);
}


/* ── Construction origin ──────────────────────────────────────────────── */

TEST(AliasMapOriginTest, ReportsHowTheMapWasBuilt)
{
    EXPECT_EQ(AliasMapGetOrigin(nullptr), ALIASMAP_ORIGIN_NONE);

    AliasMap *incremental_ptr = AliasMapCreate();

    ASSERT_NE(incremental_ptr, nullptr);
    EXPECT_EQ(AliasMapGetOrigin(incremental_ptr), ALIASMAP_ORIGIN_INCREMENTAL);

    for (int i = 0; i < 8; i++) {
        std::string key    = Key(i);
        const char *group[] = { key.c_str() };

        ASSERT_TRUE(AliasMapAdd(incremental_ptr, group, 1, "v"));
    }

    EXPECT_EQ(AliasMapGetOrigin(incremental_ptr), ALIASMAP_ORIGIN_INCREMENTAL);

    AliasMap *logger_ptr_map = AliasMapCreateWithLogger(nullptr);

    ASSERT_NE(logger_ptr_map, nullptr);
    EXPECT_EQ(AliasMapGetOrigin(logger_ptr_map), ALIASMAP_ORIGIN_INCREMENTAL);

    static const char *const group[] = { "aliased" };
    static const AliasMapCollector collectors[] = {
        { "/p/aliased", group, 1 },
    };

    AliasMapDescriptor descriptor = {};

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 1;

    AliasMap *struct_map = AliasMapCreateWithDescriptor(&descriptor);

    ASSERT_NE(struct_map, nullptr);
    EXPECT_EQ(AliasMapGetOrigin(struct_map), ALIASMAP_ORIGIN_DESCRIPTOR);

    AliasMapJsonDescriptor json_descriptor = {};

    json_descriptor.json_ptr = R"json([{"value": "/p/json", "keys": ["jsonkey"]}])json";

    AliasMap *json_map = AliasMapCreateWithJsonDescriptor(&json_descriptor);

    ASSERT_NE(json_map, nullptr);
    EXPECT_EQ(AliasMapGetOrigin(json_map), ALIASMAP_ORIGIN_JSON);

    AliasMapDestroy(incremental_ptr);
    AliasMapDestroy(logger_ptr_map);
    AliasMapDestroy(struct_map);
    AliasMapDestroy(json_map);
}

TEST_F(AliasMapTest, DescribeReportsOriginAndBytes)
{
    const char *group[] = { "a", "bb" };

    ASSERT_TRUE(AliasMapAdd(map_ptr, group, 2, "value"));

    BufferDescriptor *description = AliasMapDescribe(map_ptr, nullptr);

    ASSERT_NE(description, nullptr);
    EXPECT_NE(strstr(description->data, "\"origin\":\"incremental\""), nullptr);
    EXPECT_NE(strstr(description->data, "\"bulk\":false"), nullptr);
    EXPECT_NE(strstr(description->data, "\"key_bytes\":3"), nullptr);
    EXPECT_NE(strstr(description->data, "\"value_bytes\":5"), nullptr);

    BufferDescriptorRelease(description);
    free(description);
}

TEST_F(AliasMapDescriptorTest, DescribeReportsBulkOrigin)
{
    static const char *const group[] = { "aliased" };
    static const AliasMapCollector collectors[] = {
        { "/p/value", group, 1 },
    };

    descriptor.collectors_ptr  = collectors;
    descriptor.collector_count = 1;

    ASSERT_NE(Build(), nullptr);

    BufferDescriptor *description = AliasMapDescribe(map_ptr, nullptr);

    ASSERT_NE(description, nullptr);
    EXPECT_NE(strstr(description->data, "\"origin\":\"descriptor\""), nullptr);
    EXPECT_NE(strstr(description->data, "\"bulk\":true"), nullptr);

    BufferDescriptorRelease(description);
    free(description);
}

TEST_F(AliasMapJsonTest, DescribeReportsJsonOrigin)
{
    ASSERT_NE(Build(R"json([{"value": "/p/json", "keys": ["jsonkey"]}])json"), nullptr);

    BufferDescriptor *description = AliasMapDescribe(map_ptr, nullptr);

    ASSERT_NE(description, nullptr);
    EXPECT_NE(strstr(description->data, "\"origin\":\"json\""), nullptr);
    EXPECT_NE(strstr(description->data, "\"bulk\":true"), nullptr);

    BufferDescriptorRelease(description);
    free(description);
}

TEST_F(AliasMapJsonTest, SnapshotFollowsDocumentOrder)
{
    ASSERT_NE(Build(R"json([
        {"value": "/p/one", "keys": ["one", "uno"]},
        {"value": "/p/two", "keys": ["two"]}
    ])json"), nullptr);

    AliasMapSnapshot *snapshot_ptr = AliasMapSnapshotCreate(map_ptr);

    ASSERT_NE(snapshot_ptr, nullptr);
    EXPECT_EQ(AliasMapSnapshotGroupCount(snapshot_ptr), 2u);
    EXPECT_EQ(AliasMapSnapshotAliasCount(snapshot_ptr), 3u);
    EXPECT_EQ(SnapshotGroups(snapshot_ptr),
              (std::vector<std::string>{"/p/one{uno,one}", "/p/two{two}"}));

    AliasMapSnapshotDestroy(snapshot_ptr);
}

} // namespace
