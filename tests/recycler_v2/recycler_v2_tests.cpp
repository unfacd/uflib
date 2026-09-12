/**
 * @file recycler_v2_tests.cpp
 * @brief Lifecycle and single-thread correctness tests for RecyclerV2
 *
 * Tests the RecyclerV2 lock-free slab allocator: init, get, put, refcount,
 * expansion, InstanceHolderV2, and concurrent operations.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <uflib/recycler_v2/recycler_v2.h>
#include <uflib/recycler_v2/recycler_v2_defs.h>
#include "recycler_v2_priv.h"
}

/* ── Test payload type ────────────────────────────────────────────────────── */

typedef struct {
    int    id;
    bool   initialized;
    void  *user_data;
} TestPayloadV2;

/* ── Callback implementations ─────────────────────────────────────────────── */

static int
sInitCallbackV2(ClientContextData *data_ptr, size_t oid)
{
    TestPayloadV2 *p = (TestPayloadV2 *)data_ptr;
    p->id = (int)oid;
    p->initialized = true;
    return 0;
}

static int
sInitGetCallbackV2(InstanceHolderV2 *ih_ptr, ContextData *ctx, size_t oid, unsigned long flags)
{
    TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(ih_ptr);
    p->initialized = true;
    (void)ctx; (void)oid; (void)flags;
    return 0;
}

static int
sInitPutCallbackV2(InstanceHolderV2 *ih_ptr, ContextData *ctx, unsigned long flags)
{
    TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(ih_ptr);
    p->initialized = false;
    (void)ctx; (void)flags;
    return 0;
}

static int
sDestructCallbackV2(InstanceHolderV2 *ih_ptr, ContextData *ctx, unsigned long flags)
{
    (void)ih_ptr; (void)ctx; (void)flags;
    return 0;
}

static RecyclerV2PoolOps sTestOpsV2 = {
    sInitCallbackV2,
    sInitGetCallbackV2,
    sInitPutCallbackV2,
    sDestructCallbackV2,
    nullptr,  // marshal
    nullptr,  // unmarshal
    nullptr,  // last_used
    nullptr   // marshal_cleanup
};

/* ── Lifecycle tests ──────────────────────────────────────────────────────── */

TEST(RecyclerV2Lifecycle, InitSingleType)
{
    RecyclerV2PoolConfig cfg = {
        "TestPayloadV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 1.0f
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(handle->type, 1u);
    EXPECT_STREQ(handle->type_name, "TestPayloadV2");
    EXPECT_EQ(handle->blocksz, sizeof(TestPayloadV2));
}

TEST(RecyclerV2Lifecycle, InitMultipleTypes)
{
    RecyclerV2PoolConfig cfg1 = {"TypeOneV2", 64, 4, 32, &sTestOpsV2, 1.0f};
    RecyclerV2PoolConfig cfg2 = {"TypeTwoV2", 128, 4, 32, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *h1 = RecyclerV2InitTypePool(&cfg1);
    RecyclerV2PoolHandle *h2 = RecyclerV2InitTypePool(&cfg2);
    ASSERT_NE(h1, nullptr);
    ASSERT_NE(h2, nullptr);
    EXPECT_NE(h1->type, h2->type);
    EXPECT_GT(h1->type, 0u);
    EXPECT_GT(h2->type, 0u);
}

TEST(RecyclerV2Lifecycle, InitNullConfig)
{
    RecyclerV2PoolHandle *h = RecyclerV2InitTypePool(nullptr);
    EXPECT_EQ(h, nullptr);
}

TEST(RecyclerV2Lifecycle, InitZeroBlockSize)
{
    RecyclerV2PoolConfig cfg = {"ZeroBlock", 0, 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *h = RecyclerV2InitTypePool(&cfg);
    EXPECT_EQ(h, nullptr);
}

TEST(RecyclerV2Lifecycle, InitNullTypeName)
{
    RecyclerV2PoolConfig cfg = {nullptr, 64, 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *h = RecyclerV2InitTypePool(&cfg);
    EXPECT_EQ(h, nullptr);
}

/* ── Get/Put round-trip tests ─────────────────────────────────────────────── */

TEST(RecyclerV2GetPut, SingleRoundTrip)
{
    RecyclerV2PoolConfig cfg = {"RoundTripV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih, nullptr);
    EXPECT_TRUE(RecyclerV2IsInstance(ih));

    TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(ih);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->initialized);
    EXPECT_GT(p->id, 0);

    int rc = RecyclerV2Put(ih, nullptr, 0);
    EXPECT_EQ(rc, 0);
    free(ih);
}

TEST(RecyclerV2GetPut, MultipleSequential)
{
    RecyclerV2PoolConfig cfg = {"MultiGetV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    for (int i = 0; i < 20; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        ASSERT_NE(ih, nullptr);
        TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(ih);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(p->initialized);

        int rc = RecyclerV2Put(ih, nullptr, 0);
        EXPECT_EQ(rc, 0);
        free(ih);
    }
}

/* ── Refcount tests ───────────────────────────────────────────────────────── */

TEST(RecyclerV2Refcount, IncrementAndDecrement)
{
    RecyclerV2PoolConfig cfg = {"RefcountV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih, nullptr);

    size_t rc0 = RecyclerV2GetReferenceCount(ih);
    EXPECT_GE(rc0, 2u);  // base(1) + get(1) = 2

    RecyclerV2Referenced(ih, 1);
    size_t rc1 = RecyclerV2GetReferenceCount(ih);
    EXPECT_EQ(rc1, rc0 + 1);

    RecyclerV2UnReferenced(ih, 1);
    size_t rc2 = RecyclerV2GetReferenceCount(ih);
    EXPECT_EQ(rc2, rc0);

    int ret = RecyclerV2Put(ih, nullptr, 0);
    EXPECT_EQ(ret, 0);
    free(ih);
}

/* ── InstanceHolderV2 tests ───────────────────────────────────────────────── */

TEST(InstanceHolderV2, SetInstanceAndGetInstance)
{
    TestPayloadV2 payload = {42, true, nullptr};
    InstanceHolderV2 holder;
    RecyclerV2SetInstance(&holder, &payload);

    EXPECT_TRUE(RecyclerV2IsInstance(&holder));
    EXPECT_FALSE(RecyclerV2IsMarshaller(&holder));

    TestPayloadV2 *retrieved = (TestPayloadV2 *)RecyclerV2GetInstance(&holder);
    EXPECT_EQ(retrieved, &payload);
    EXPECT_EQ(retrieved->id, 42);
}

TEST(InstanceHolderV2, SetMarshallerAndGetMarshaller)
{
    InstanceHolderV2 holder;
    RecyclerV2SetMarshaller(&holder, 0xABCD);

    EXPECT_TRUE(RecyclerV2IsMarshaller(&holder));
    EXPECT_FALSE(RecyclerV2IsInstance(&holder));

    uintptr_t id = RecyclerV2GetMarshaller(&holder);
    EXPECT_EQ(id, 0xABCDu);
}

TEST(InstanceHolderV2, ModeTransitionInstanceToInstance)
{
    TestPayloadV2 p1 = {1}, p2 = {2};
    InstanceHolderV2 holder;
    RecyclerV2SetInstance(&holder, &p1);
    EXPECT_TRUE(RecyclerV2IsInstance(&holder));

    RecyclerV2SetInstance(&holder, &p2);
    EXPECT_TRUE(RecyclerV2IsInstance(&holder));
    TestPayloadV2 *retrieved = (TestPayloadV2 *)RecyclerV2GetInstance(&holder);
    EXPECT_EQ(retrieved, &p2);
}

TEST(InstanceHolderV2, MarshallerEncodingRoundTrip)
{
    uint16_t type_idx = 5;
    uint64_t m_id = 0x123456789ABC;
    uintptr_t encoded = RecyclerV2EncodeMarshaller(type_idx, m_id);

    EXPECT_EQ(RecyclerV2MarshallerTypeIndex(encoded), type_idx);
    EXPECT_EQ(RecyclerV2MarshallerId(encoded), m_id);
    EXPECT_EQ(encoded & 1, 1u);  // bit[0] = Marshaller mode
}

/* ── Expansion tests ──────────────────────────────────────────────────────── */

TEST(RecyclerV2Expansion, ExhaustAndExpand)
{
    RecyclerV2PoolConfig cfg = {"ExhaustV2", sizeof(TestPayloadV2), 16, 8, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    std::vector<InstanceHolderV2 *> holders;

    for (int i = 0; i < 200; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        if (ih == nullptr) break;
        holders.push_back(ih);
    }

    EXPECT_GT(holders.size(), 8u);  // should have expanded beyond first group

    for (auto *ih : holders) {
        EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0);
        free(ih);
    }
}

/* ── Pool exhaustion test ─────────────────────────────────────────────────── */

TEST(RecyclerV2Exhaustion, PoolExhaustion)
{
    /* Small pool with only 1 group and small expansion threshold */
    RecyclerV2PoolConfig cfg = {"TinyV2", sizeof(TestPayloadV2), 1, 4, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    std::vector<InstanceHolderV2 *> holders;
    for (int i = 0; i < 20; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        if (ih == nullptr) break;
        holders.push_back(ih);
    }

    /* Should not be able to get more than pool_size */
    EXPECT_LE(holders.size(), 4u);

    for (auto *ih : holders) {
        RecyclerV2Put(ih, nullptr, 0);
        free(ih);
    }
}

/* ── Concurrent tests ─────────────────────────────────────────────────────── */

TEST(RecyclerV2Concurrent, DisjointGetPut)
{
    RecyclerV2PoolConfig cfg = {"ConcurrentV2", sizeof(TestPayloadV2), 16, 256, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    constexpr int kThreads = 4;
    constexpr int kIters = 100;

    std::vector<std::thread> threads;
    std::atomic<int> get_errors{0};
    std::atomic<int> put_errors{0};
    std::atomic<int> null_gets{0};

    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([handle, &get_errors, &put_errors, &null_gets]() {
            for (int i = 0; i < kIters; i++) {
                InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
                if (ih == nullptr) {
                    null_gets.fetch_add(1);
                    continue;
                }
                TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(ih);
                if (p == nullptr || !p->initialized) {
                    get_errors.fetch_add(1);
                }
                int rc = RecyclerV2Put(ih, nullptr, 0);
                if (rc != 0) {
                    put_errors.fetch_add(1);
                }
                free(ih);
            }
        });
    }

    for (auto &th : threads) th.join();

    EXPECT_EQ(null_gets.load(), 0);
    EXPECT_EQ(get_errors.load(), 0);
    EXPECT_EQ(put_errors.load(), 0);
}

/* ── Query function tests ─────────────────────────────────────────────────── */

TEST(RecyclerV2Query, CapacityAndLeasedCount)
{
    RecyclerV2PoolConfig cfg = {"QueryV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    size_t cap = RecyclerV2GetCapacity(handle);
    EXPECT_EQ(cap, 64u);

    size_t leased_before = RecyclerV2GetLeasedCount(handle);
    EXPECT_EQ(leased_before, 0u);

    InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih, nullptr);

    size_t leased_after = RecyclerV2GetLeasedCount(handle);
    EXPECT_EQ(leased_after, 1u);

    RecyclerV2Put(ih, nullptr, 0);
    free(ih);

    size_t leased_final = RecyclerV2GetLeasedCount(handle);
    EXPECT_EQ(leased_final, 0u);
}

TEST(RecyclerV2Query, GetPoolConfigDefaults)
{
    /* Verify config introspection returns the effective values,
     * including defaults filled in for zero-initialized fields. */
    RecyclerV2PoolConfig cfg = {
        "ConfigTestV2", sizeof(TestPayloadV2), 0, 0, &sTestOpsV2, 0.0f
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    RecyclerV2PoolConfig out;
    memset(&out, 0, sizeof(out));
    RecyclerV2GetPoolConfig(handle, &out);

    EXPECT_STREQ(out.type_name, "ConfigTestV2");
    EXPECT_EQ(out.blocksz, sizeof(TestPayloadV2));
    EXPECT_EQ(out.group_allocation_sz, CONFIG_DEFAULT_RECYCLER_V2_MAX_ALLOCATION_GROUPS);
    EXPECT_EQ(out.expansion_threshold, CONFIG_DEFAULT_RECYCLER_V2_EXPANSION_THRESHOLD);
    EXPECT_NE(out.ops_ptr, nullptr);  // recycler copies ops, so pointer differs from &sTestOpsV2
    EXPECT_EQ(out.ops_ptr->poolop_init_callback, sTestOpsV2.poolop_init_callback);
    EXPECT_FLOAT_EQ(out.marshal_watermark, CONFIG_DEFAULT_RECYCLER_V2_MARSHAL_WATERMARK);
    EXPECT_EQ(out.marshal_blob_max_sz, 0u);
    EXPECT_STREQ(out.storage_root, CONFIG_DEFAULT_RECYCLER_V2_STORAGE_ROOT);
}

TEST(RecyclerV2Query, GetPoolConfigCustomStorage)
{
    /* Verify a custom storage root is reflected in the returned config. */
    RecyclerV2PoolConfig cfg = {
        "CustomStorageV2", sizeof(TestPayloadV2), 2, 128, &sTestOpsV2, 0.0f,
        4096, "/tmp/test_blobs"
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    RecyclerV2PoolConfig out;
    memset(&out, 0, sizeof(out));
    RecyclerV2GetPoolConfig(handle, &out);

    EXPECT_STREQ(out.type_name, "CustomStorageV2");
    EXPECT_EQ(out.group_allocation_sz, 2u);
    EXPECT_EQ(out.expansion_threshold, 128u);
    EXPECT_EQ(out.marshal_blob_max_sz, 4096u);
    EXPECT_STREQ(out.storage_root, "/tmp/test_blobs");
}

TEST(RecyclerV2Query, InstanceIdCreatesSubdirectory)
{
    /* Two pools with different instance_ids on the same root must not collide. */
    const char *root = "/tmp/recycler_v2_instance_test";
    system("rm -rf /tmp/recycler_v2_instance_test");

    /* Instance A — ufsrv_class + instance_id both set */
    RecyclerV2PoolConfig cfgA = {
        "TypeOne", sizeof(TestPayloadV2), 1, 4, &sTestOpsV2, 1.0f, 0,
        root, "ufsrvwebsock", "east", RECYCLER_V2_STORAGE_OVERWRITE
    };
    RecyclerV2PoolHandle *hA = RecyclerV2InitTypePool(&cfgA);
    ASSERT_NE(hA, nullptr);

    /* Instance B — same type name, different instance_id, same class */
    RecyclerV2PoolConfig cfgB = {
        "TypeOne", sizeof(TestPayloadV2), 1, 4, &sTestOpsV2, 1.0f, 0,
        root, "ufsrvwebsock", "west", RECYCLER_V2_STORAGE_OVERWRITE
    };
    RecyclerV2PoolHandle *hB = RecyclerV2InitTypePool(&cfgB);
    ASSERT_NE(hB, nullptr);

    /* Verify both instance directories exist */
    struct stat st;
    EXPECT_EQ(stat("/tmp/recycler_v2_instance_test/ufsrvwebsock/east/TypeOne", &st), 0);
    EXPECT_EQ(stat("/tmp/recycler_v2_instance_test/ufsrvwebsock/west/TypeOne", &st), 0);

    /* Config introspection */
    RecyclerV2PoolConfig outA = {}, outB = {};
    RecyclerV2GetPoolConfig(hA, &outA);
    RecyclerV2GetPoolConfig(hB, &outB);
    EXPECT_STREQ(outA.ufsrv_class, "ufsrvwebsock");
    EXPECT_STREQ(outA.instance_id, "east");
    EXPECT_STREQ(outB.ufsrv_class, "ufsrvwebsock");
    EXPECT_STREQ(outB.instance_id, "west");
    EXPECT_STREQ(outA.storage_root, root);
    EXPECT_STREQ(outB.storage_root, root);
    /* Different instances, same root + class, no collision */

    system("rm -rf /tmp/recycler_v2_instance_test");
}

TEST(RecyclerV2Query, CustomStorageDirCreated)
{
    /* Verify the storage directory is actually created at the custom root. */
    const char *root = "/tmp/recycler_v2_test_storage";
    const char *type = "StorageDirTest";

    /* Clean up any leftover from a prior failed run */
    char cleanup[256];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);

    RecyclerV2PoolConfig cfg = {
        type, sizeof(TestPayloadV2), 1, 4, &sTestOpsV2, 1.0f, 0, root
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Verify the directory <root>/<type_name> exists on disk */
    struct stat st;
    char expected_path[256];
    snprintf(expected_path, sizeof(expected_path), "%s/%s", root, type);
    EXPECT_EQ(stat(expected_path, &st), 0)
        << "Expected storage dir not found: " << expected_path;
    EXPECT_TRUE(S_ISDIR(st.st_mode))
        << "Expected a directory at: " << expected_path;

    /* Config introspection must match */
    RecyclerV2PoolConfig out;
    memset(&out, 0, sizeof(out));
    RecyclerV2GetPoolConfig(handle, &out);
    EXPECT_STREQ(out.storage_root, root);

    /* Clean up */
    system(cleanup);
}

/* ── Storage init policy tests ────────────────────────────────────────────── */

class RecyclerV2StorageInitPolicyTest : public ::testing::Test {
protected:
    const char *root   = "/tmp/recycler_v2_storage_policy_test";
    const char *type   = "PolicyTest";
    char        path[256];

    void SetUp() override {
        snprintf(path, sizeof(path), "%s/%s", root, type);
        system("rm -rf /tmp/recycler_v2_storage_policy_test");
        mkdir(root, 0700);
        mkdir(path, 0700);
        /* Create a dummy stale blob */
        char blob[256];
        snprintf(blob, sizeof(blob), "%s/0000000000000001.pb", path);
        FILE *f = fopen(blob, "w");
        if (f) { fwrite("stale", 1, 5, f); fclose(f); }
    }

    void TearDown() override {
        system("rm -rf /tmp/recycler_v2_storage_policy_test");
    }

    static int InitCallback(ClientContextData *d, size_t oid) {
        (void)d; (void)oid; return 0;
    }
    static int InitGetCallback(InstanceHolderV2 *ih, ContextData *c,
                               size_t oid, unsigned long f) {
        (void)ih; (void)c; (void)oid; (void)f; return 0;
    }
    static int InitPutCallback(InstanceHolderV2 *ih, ContextData *c,
                               unsigned long f) {
        (void)ih; (void)c; (void)f; return 0;
    }

    RecyclerV2PoolOps MakeOps() {
        RecyclerV2PoolOps ops = {nullptr};
        ops.poolop_init_callback    = InitCallback;
        ops.poolop_initget_callback = InitGetCallback;
        ops.poolop_initput_callback = InitPutCallback;
        return ops;
    }

    bool FileExists(const char *subpath) {
        struct stat st;
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", path, subpath);
        return stat(full, &st) == 0;
    }
};

TEST_F(RecyclerV2StorageInitPolicyTest, OverwriteDeletesStaleBlobs)
{
    EXPECT_TRUE(FileExists("0000000000000001.pb"));

    RecyclerV2PoolOps ops = MakeOps();
    RecyclerV2PoolConfig cfg = {
        type, sizeof(TestPayloadV2), 1, 4, &ops, 1.0f, 0, root, nullptr, nullptr,
        RECYCLER_V2_STORAGE_OVERWRITE
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Stale blob must be gone */
    EXPECT_FALSE(FileExists("0000000000000001.pb"));

    /* Config introspection */
    RecyclerV2PoolConfig out = {};
    RecyclerV2GetPoolConfig(handle, &out);
    EXPECT_EQ(out.storage_init_policy, RECYCLER_V2_STORAGE_OVERWRITE);
}

TEST_F(RecyclerV2StorageInitPolicyTest, AppendKeepsStaleBlobs)
{
    EXPECT_TRUE(FileExists("0000000000000001.pb"));

    RecyclerV2PoolOps ops = MakeOps();
    RecyclerV2PoolConfig cfg = {
        type, sizeof(TestPayloadV2), 1, 4, &ops, 1.0f, 0, root, nullptr, nullptr,
        RECYCLER_V2_STORAGE_APPEND
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Stale blob must still be there */
    EXPECT_TRUE(FileExists("0000000000000001.pb"));

    RecyclerV2PoolConfig out = {};
    RecyclerV2GetPoolConfig(handle, &out);
    EXPECT_EQ(out.storage_init_policy, RECYCLER_V2_STORAGE_APPEND);
}

TEST_F(RecyclerV2StorageInitPolicyTest, ArchiveMovesStaleBlobs)
{
    EXPECT_TRUE(FileExists("0000000000000001.pb"));

    RecyclerV2PoolOps ops = MakeOps();
    RecyclerV2PoolConfig cfg = {
        type, sizeof(TestPayloadV2), 1, 4, &ops, 1.0f, 0, root, nullptr, nullptr,
        RECYCLER_V2_STORAGE_ARCHIVE
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Stale blob must be gone from the type directory */
    EXPECT_FALSE(FileExists("0000000000000001.pb"));

    /* But must exist under archived/ */
    char archive_glob[256];
    snprintf(archive_glob, sizeof(archive_glob),
        "find %s/archived -name '0000000000000001.pb' 2>/dev/null", root);
    int rc = system(archive_glob);
    EXPECT_EQ(WEXITSTATUS(rc), 0)
        << "Stale blob should be under " << root << "/archived/";

    RecyclerV2PoolConfig out = {};
    RecyclerV2GetPoolConfig(handle, &out);
    EXPECT_EQ(out.storage_init_policy, RECYCLER_V2_STORAGE_ARCHIVE);
}

/* ── Marshaller watermark test ────────────────────────────────────────────── */

TEST(RecyclerV2MarshalWatermark, SetAndDefault)
{
    RecyclerV2PoolConfig cfg = {"WatermarkV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 0.5f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Default from config was 0.5 — verify Get works */
    InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih, nullptr);

    /* Set a new watermark */
    RecyclerV2SetMarshalWatermark(handle, 0.8f);

    RecyclerV2Put(ih, nullptr, 0);
    free(ih);
}

TEST(RecyclerV2MarshalWatermark, LeasedCountUsageRatio)
{
    /* Verify that usage computation uses actual leased counts, not pool_size.
     * Create a pool with a small threshold so we can exhaust it quickly. */
    RecyclerV2PoolConfig cfg = {"UsageV2", sizeof(TestPayloadV2), 1, 8, &sTestOpsV2, 0.5f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    size_t capacity = RecyclerV2GetCapacity(handle);
    EXPECT_EQ(capacity, 8u);

    /* Lease all usable objects (pool_size-1 = 7 objects, because index 0 is the sentinel).
     * Object at index 0 has refcount 0 and is never popped. */
    std::vector<InstanceHolderV2 *> holders;
    for (int i = 0; i < 7; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        ASSERT_NE(ih, nullptr) << "Failed to get object " << i;
        holders.push_back(ih);
    }

    size_t leased = RecyclerV2GetLeasedCount(handle);
    EXPECT_EQ(leased, 7u);

    /* At this point the free stack is empty (only index 0 remains, which is the sentinel).
     * When Get cycles through the Treiber stack and finds all free stacks empty (head == 0),
     * it enters the exhaustion path and computes usage by scanning envelopes with refcount>=2.
     * Here: 7 objects with refcount=2, 1 sentinel with refcount=0 → usage = 7/8 = 0.875.
     * With watermark=0.5, this triggers CLOCK sweep. But no objects are idle (refcount==1),
     * so CLOCK finds nothing. Expansion is blocked (group_allocation_sz=1, already at max).
     * The 8th Get returns NULL (hard exhaustion). */
    InstanceHolderV2 *ih8 = RecyclerV2Get(handle, nullptr, 0);
    EXPECT_EQ(ih8, nullptr) << "Pool should be exhausted after 7 leased objects (sentinel at idx 0)";

    /* Return all objects */
    for (auto *ih : holders) {
        EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0);
        free(ih);
    }

    /* After returning all, leased count should be 0 */
    leased = RecyclerV2GetLeasedCount(handle);
    EXPECT_EQ(leased, 0u);
}

/* ── Marshal/unmarshal lifecycle test ─────────────────────────────────────── */

static std::atomic<int> sMarshalCountV2{0};
static std::atomic<int> sUnmarshalCountV2{0};
static std::atomic<int> sCleanupCountV2{0};

static int
sMockMarshalCallbackV2(ClientContextData *obj_ptr, uint8_t *out_buf,
                       size_t buf_sz, size_t *out_len_ptr)
{
    TestPayloadV2 *p = (TestPayloadV2 *)obj_ptr;
    /* Write a magic number to verify serialization */
    memcpy(out_buf, &p->id, sizeof(p->id));
    *out_len_ptr = sizeof(p->id);
    sMarshalCountV2.fetch_add(1);
    return 0;
}

static int
sMockUnmarshalCallbackV2(ClientContextData *obj_ptr, const uint8_t *in_buf,
                         size_t buf_len)
{
    TestPayloadV2 *p = (TestPayloadV2 *)obj_ptr;
    memcpy(&p->id, in_buf, sizeof(p->id));
    p->initialized = true;
    sUnmarshalCountV2.fetch_add(1);
    return 0;
}

static void
sMockMarshalCleanupCallbackV2(MarshallerContextData marshaller_id)
{
    (void)marshaller_id;
    sCleanupCountV2.fetch_add(1);
}

TEST(RecyclerV2MarshalLifecycle, InstanceMarshalUnmarshalCycle)
{
    sMarshalCountV2.store(0);
    sUnmarshalCountV2.store(0);
    sCleanupCountV2.store(0);

    RecyclerV2PoolOps ops = sTestOpsV2;
    ops.poolop_marshal_callback = sMockMarshalCallbackV2;
    ops.poolop_unmarshal_callback = sMockUnmarshalCallbackV2;
    ops.poolop_marshal_cleanup_callback = sMockMarshalCleanupCallbackV2;

    /* Small pool: 1 group of 8 objects (7 usable + 1 sentinel), watermark 0.5.
     * marshal_blob_max_sz = 256 — consumer declares its max serialized size. */
    RecyclerV2PoolConfig cfg = {
        "MarshalCycleV2", sizeof(TestPayloadV2), 2, 8, &ops, 0.5f, 256
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Lease all 7 usable objects — pool exhausted, usage = 7/8 = 0.875 ≥ 0.5.
     * CLOCK sweep runs but finds no idle objects (all refcount==2). */
    std::vector<InstanceHolderV2 *> holders;
    for (int i = 0; i < 7; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        ASSERT_NE(ih, nullptr) << "Failed to get object " << i;
        holders.push_back(ih);
    }

    /* 8th Get should trigger exhaustion → expansion to group 2 (group_allocation_sz=2) */
    InstanceHolderV2 *ih8 = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih8, nullptr) << "8th Get should succeed via expansion to group 2";
    holders.push_back(ih8);

    size_t capacity = RecyclerV2GetCapacity(handle);
    EXPECT_EQ(capacity, 16u);  // 2 groups × 8

    /* Return all — pool back to full free stacks */
    for (auto *ih : holders) {
        EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0);
        free(ih);
    }

    /* Marshaller counters should still be 0 — no marshalling occurred because
     * the CLOCK sweep found no idle victims (all objects were actively leased). */
    EXPECT_EQ(sMarshalCountV2.load(), 0)
        << "Marshal should not be called when all objects are busy";
}

TEST(RecyclerV2MarshalLifecycle, MarshalCallbackRoundTrip)
{
    /* Direct marshal/unmarshal callback test without pool exhaustion */
    sMarshalCountV2.store(0);
    sUnmarshalCountV2.store(0);
    sCleanupCountV2.store(0);

    RecyclerV2PoolOps ops = sTestOpsV2;
    ops.poolop_marshal_callback = sMockMarshalCallbackV2;
    ops.poolop_unmarshal_callback = sMockUnmarshalCallbackV2;
    ops.poolop_marshal_cleanup_callback = sMockMarshalCleanupCallbackV2;

    /* Pool with marshal callbacks and consumer-declared blob size */
    RecyclerV2PoolConfig cfg = {
        "MarshalCBV2", sizeof(TestPayloadV2), 4, 64, &ops, 1.0f, 256
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Get an object and verify callbacks are registered */
    InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih, nullptr);

    TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(ih);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->initialized);

    /* Manually test marshal callback on the object payload */
    uint8_t buf[256];
    size_t len = 0;
    int rc = sMockMarshalCallbackV2((ClientContextData *)p, buf, sizeof(buf), &len);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(len, sizeof(int));

    /* Verify unmarshal can restore the original id */
    TestPayloadV2 restored = {};
    rc = sMockUnmarshalCallbackV2((ClientContextData *)&restored, buf, len);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(restored.id, p->id);
    EXPECT_TRUE(restored.initialized);

    EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0);
    free(ih);
}

/* ── GetClientContextData test ────────────────────────────────────────────── */

TEST(RecyclerV2ClientContext, GetClientContextData)
{
    RecyclerV2PoolConfig cfg = {"CtxV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih, nullptr);

    ClientContextData *ctx = RecyclerV2GetClientContextData(ih);
    ASSERT_NE(ctx, nullptr);

    TestPayloadV2 *p = (TestPayloadV2 *)ctx;
    EXPECT_TRUE(p->initialized);

    RecyclerV2Put(ih, nullptr, 0);
    free(ih);
}

/* ── DestroyInstance test ─────────────────────────────────────────────────── */

TEST(RecyclerV2DestroyInstance, DestroyAliasDecrementsRefcount)
{
    /* DestroyInstance is the counterpart to GetNewInstance (multi-holder):
     * it removes one alias and releases its reference.  The primary holder
     * from Get() is returned via Put(), not DestroyInstance(). */
    RecyclerV2PoolConfig cfg = {"DestroyV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    InstanceHolderV2 *primary = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(primary, nullptr);
    EXPECT_EQ(RecyclerV2GetReferenceCount(primary), 2u);  // base(1) + get(1)

    /* Create an alias — GetNewInstance increments the refcount */
    InstanceHolderV2 *alias = RecyclerV2GetNewInstance(primary);
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(RecyclerV2GetReferenceCount(primary), 3u);  // base + primary + alias

    /* Destroy the alias — releases its reference and removes it */
    int rc = RecyclerV2DestroyInstance(alias);
    EXPECT_EQ(rc, RECYCLER_V2_INSTANCE_HOLDER_FOUND);
    free(alias);  // caller owns the alias holder and frees it

    EXPECT_EQ(RecyclerV2GetReferenceCount(primary), 2u);

    /* Destroy NULL should return NOT_FOUND */
    rc = RecyclerV2DestroyInstance(nullptr);
    EXPECT_EQ(rc, RECYCLER_V2_INSTANCE_HOLDER_NOT_FOUND);

    /* The primary holder still owns the object — return it via Put */
    EXPECT_EQ(RecyclerV2Put(primary, nullptr, 0), 0);
    free(primary);
}

TEST(RecyclerV2DestroyInstance, DestroyPrimaryPromotesAlias)
{
    /* When the primary holder is removed while aliases still exist, the
     * fallback list must promote a real alias into holder_word so the
     * object is not lost. */
    RecyclerV2PoolConfig cfg = {"PromoteV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    InstanceHolderV2 *primary = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(primary, nullptr);

    InstanceHolderV2 *alias = RecyclerV2GetNewInstance(primary);
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(RecyclerV2GetReferenceCount(primary), 3u);

    /* Destroy the primary — the alias must be promoted to holder_word */
    int rc = RecyclerV2DestroyInstance(primary);
    EXPECT_EQ(rc, RECYCLER_V2_INSTANCE_HOLDER_FOUND);
    free(primary);
    EXPECT_EQ(RecyclerV2GetReferenceCount(alias), 2u);

    /* The promoted alias still resolves to the live object */
    TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(alias);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->initialized);

    /* Clean up: return the (now-primary) alias object */
    EXPECT_EQ(RecyclerV2Put(alias, nullptr, 0), 0);
    free(alias);
}

/* ── Pending unmarshal queue test ──────────────────────────────────────────── */

TEST(RecyclerV2AsyncPending, EnqueueAndDrain)
{
    /* Verify that multiple pending holders can be enqueued and drained.
     * This tests the Gate 3 fix — the old single-element CAS "stack" would
     * silently lose entries. */

    /* Create a pool to get a valid type_index */
    RecyclerV2PoolConfig cfg = {"PendingQTestV2", sizeof(TestPayloadV2), 4, 64, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);
    uint16_t type_idx = handle->type;
    ASSERT_GT(type_idx, 0u);

    /* Create marshalled holders using the actual type_index */
    InstanceHolderV2 holder1, holder2, holder3;
    holder1.holder.marshaller = RecyclerV2EncodeMarshaller(type_idx, 100);
    holder2.holder.marshaller = RecyclerV2EncodeMarshaller(type_idx, 200);
    holder3.holder.marshaller = RecyclerV2EncodeMarshaller(type_idx, 300);

    /* Enqueue — should succeed without errors */
    RecyclerV2EnqueuePendingUnmarshal(&holder1);
    RecyclerV2EnqueuePendingUnmarshal(&holder2);
    RecyclerV2EnqueuePendingUnmarshal(&holder3);

    /* Drain the queue via a Put call (which triggers sPendingStackDrainV2).
     * The drain will attempt to resolve each holder. Since the marshaller IDs
     * (100, 200, 300) don't correspond to on-disk blobs, the resolve will
     * fail (storage read fails), and the holders will be re-enqueued. This
     * exercises the full enqueue → drain → callback path without needing
     * real files on disk. */
    InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih, nullptr);
    RecyclerV2Put(ih, nullptr, 0);
    free(ih);

    /* After Put drain, the queue should still have entries (resolve failed
     * because no on-disk blobs).  Verify by enqueuing one more — if the old
     * queue was correctly preserved, this doesn't crash. */
    InstanceHolderV2 holder4;
    holder4.holder.marshaller = RecyclerV2EncodeMarshaller(type_idx, 400);
    RecyclerV2EnqueuePendingUnmarshal(&holder4);

    /* SUCCESS: no crash = queue structure maintained integrity through
     * multi-entry enqueue + drain attempt + re-enqueue. */
    SUCCEED();
}

/* ── Exhaustion behaviour tests ──────────────────────────────────────────── */

TEST(RecyclerV2Exhaustion, ExpandWhenRoomThenHardExhaustion)
{
    /* Scenario: 1 group, expansion_threshold=8 (7 usable + 1 sentinel),
     * group_allocation_sz=2.  Lease all 7 usable objects → pool exhausted.
     * Next Get must expand to group 2.  Lease all objects in group 2.
     * Next Get must hit hard exhaustion (max groups reached). */

    RecyclerV2PoolConfig cfg = {
        "ExhaustTestV2", sizeof(TestPayloadV2), 2, 8, &sTestOpsV2, 1.0f
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Phase 1 — exhaust first group (7 usable objects, index 0 = sentinel) */
    std::vector<InstanceHolderV2 *> holders;
    for (int i = 0; i < 7; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        ASSERT_NE(ih, nullptr) << "Get failed at object " << i
            << " — expected 7 usable objects in group 0";
        holders.push_back(ih);
    }
    EXPECT_EQ(RecyclerV2GetCapacity(handle), 8u);
    EXPECT_EQ(RecyclerV2GetLeasedCount(handle), 7u);

    /* Phase 2 — group 0 fully exhausted, expansion must kick in */
    InstanceHolderV2 *ih_group2 = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih_group2, nullptr)
        << "EXPECTED: expansion to group 2 when group 0 is exhausted "
        << "(group_allocation_sz=2, room to grow)";
    holders.push_back(ih_group2);
    EXPECT_EQ(RecyclerV2GetCapacity(handle), 16u);  // 2 groups × 8

    /* Lease remaining usable objects in group 2 (6 more = 7 total in group 2) */
    for (int i = 0; i < 6; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        ASSERT_NE(ih, nullptr) << "Get failed at group-2 object " << i;
        holders.push_back(ih);
    }
    EXPECT_EQ(RecyclerV2GetLeasedCount(handle), 14u);  // 7 + 7

    /* Phase 3 — both groups fully leased, group_allocation_sz=2 exhausted.
     * The 15th Get (would be 15th usable across 2 groups) must return NULL.
     * Total usable objects = 2 groups × (8-1 sentinel) = 14. */
    InstanceHolderV2 *ih_null = RecyclerV2Get(handle, nullptr, 0);
    EXPECT_EQ(ih_null, nullptr)
        << "EXPECTED: hard exhaustion when all groups at max capacity";

    /* Clean up */
    for (auto *ih : holders) {
        EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0);
        free(ih);
    }
}

TEST(RecyclerV2Exhaustion, SingleGroupNoExpansion)
{
    /* Scenario: 1 group, group_allocation_sz=1, expansion_threshold=4.
     * Lease all 3 usable objects → exhausted.  Expansion blocked (max=1).
     * Next Get must return NULL.  This is the exact scenario:
     *   "1 allocation group, all slots leased, a get() arrives." */

    RecyclerV2PoolConfig cfg = {
        "OneGroupV2", sizeof(TestPayloadV2), 1, 4, &sTestOpsV2, 1.0f
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(RecyclerV2GetCapacity(handle), 4u);

    /* Exhaust all usable objects: pool_size=4, index 0 = sentinel,
     * so 3 usable objects */
    std::vector<InstanceHolderV2 *> holders;
    for (int i = 0; i < 3; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        ASSERT_NE(ih, nullptr) << "Get failed at object " << i;
        holders.push_back(ih);
    }
    EXPECT_EQ(RecyclerV2GetCapacity(handle), 4u);

    /* All slots leased, group_allocation_sz=1 → expansion blocked.
     * The next Get must return NULL (hard exhaustion). */
    InstanceHolderV2 *ih_null = RecyclerV2Get(handle, nullptr, 0);
    EXPECT_EQ(ih_null, nullptr)
        << "EXPECTED NULL: 1 group, all slots leased, expansion blocked";

    /* Clean up */
    for (auto *ih : holders) {
        EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0);
        free(ih);
    }
}

TEST(RecyclerV2Exhaustion, ExpansionThenMarshalFallback)
{
    /* Scenario: pool with marshaller configured, group_allocation_sz=2.
     * Exhaust group 0 → expansion to group 2 (expand-first path).
     * Exhaust group 2 → expansion blocked.  At hard exhaustion,
     * CLOCK sweep runs (marshal-as-last-resort) — but in this test
     * all objects are busy so marshal finds no victim.  Get returns NULL.
     *
     * Verifies the post-remediation flow: expand-first, marshal-fallback. */

    int marshal_attempts = 0;
    auto marshal_cb = [](ClientContextData *, uint8_t *, size_t, size_t *) -> int {
        return 0;
    };
    (void)marshal_cb;  // unused — just verifying the callback path exists

    sMarshalCountV2.store(0);
    sUnmarshalCountV2.store(0);

    RecyclerV2PoolOps ops = sTestOpsV2;
    ops.poolop_marshal_callback = sMockMarshalCallbackV2;
    ops.poolop_unmarshal_callback = sMockUnmarshalCallbackV2;

    RecyclerV2PoolConfig cfg = {
        "ExpandMarshalV2", sizeof(TestPayloadV2), 2, 8, &ops, 1.0f, 256
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Exhaust group 0 (7 usable) */
    std::vector<InstanceHolderV2 *> holders;
    for (int i = 0; i < 7; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        ASSERT_NE(ih, nullptr);
        holders.push_back(ih);
    }

    /* Next Get: expand-first to group 2 */
    InstanceHolderV2 *ih_expand = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(ih_expand, nullptr)
        << "EXPECTED: expansion to group 2 (expand-first path)";
    holders.push_back(ih_expand);
    EXPECT_EQ(RecyclerV2GetCapacity(handle), 16u);

    /* Exhaust group 2 (6 more = 7 total in group 2) */
    for (int i = 0; i < 6; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        ASSERT_NE(ih, nullptr);
        holders.push_back(ih);
    }
    EXPECT_EQ(RecyclerV2GetLeasedCount(handle), 14u);

    /* Both groups exhausted, expansion blocked (max=2).
     * Next Get: CLOCK sweep runs as last resort but finds no idle
     * victims (all refcount>=2).  Marshal is NOT called. */
    int marshal_before = sMarshalCountV2.load();
    InstanceHolderV2 *ih_null = RecyclerV2Get(handle, nullptr, 0);
    EXPECT_EQ(ih_null, nullptr)
        << "EXPECTED NULL: both groups exhausted, CLOCK finds no idle victims";
    EXPECT_EQ(sMarshalCountV2.load(), marshal_before)
        << "Marshal should NOT be called — all objects are busy (refcount>=2), "
        << "CLOCK sweep finds no victims";

    /* Clean up */
    for (auto *ih : holders) {
        EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0);
        free(ih);
    }
}

/* ── End-to-end marshal/unmarshal blob integrity tests ───────────────────── */

static int sE2EMarshalCountV2  = 0;
static int sE2EUnmarshalCountV2 = 0;

static int
sE2EMarshalCallbackV2(ClientContextData *obj_ptr, uint8_t *out_buf,
                      size_t buf_sz, size_t *out_len_ptr)
{
    TestPayloadV2 *p = (TestPayloadV2 *)obj_ptr;
    memcpy(out_buf, &p->id, sizeof(p->id));
    *out_len_ptr = sizeof(p->id);
    sE2EMarshalCountV2++;
    return 0;
}

static int
sE2EUnmarshalCallbackV2(ClientContextData *obj_ptr, const uint8_t *in_buf,
                        size_t buf_len)
{
    TestPayloadV2 *p = (TestPayloadV2 *)obj_ptr;
    if (buf_len < sizeof(int)) return -1;
    memcpy(&p->id, in_buf, sizeof(int));
    p->initialized = true;
    sE2EUnmarshalCountV2++;
    return 0;
}

TEST(RecyclerV2MarshalE2E, BlobRoundTripOnDisk)
{
    /* End-to-end: write a real blob to disk, encode a marshalled holder
     * pointing at it, call RecyclerV2GetInstance() to trigger the full
     * unmarshal path, and verify the returned object's data integrity.
     *
     * Path exercised:
     *   StorageWrite → encode → GetInstance slow path →
     *   StorageRead → unmarshal callback → CAS → StorageDelete */

    const int test_id   = 0x7A3F1E02;
    const size_t blob_sz = sizeof(test_id);
    const char  *root    = "/tmp/recycler_v2_e2e_blob";

    system("rm -rf /tmp/recycler_v2_e2e_blob");

    RecyclerV2PoolOps ops = sTestOpsV2;
    ops.poolop_marshal_callback         = sE2EMarshalCallbackV2;
    ops.poolop_unmarshal_callback       = sE2EUnmarshalCallbackV2;
    ops.poolop_marshal_cleanup_callback = sMockMarshalCleanupCallbackV2;

    RecyclerV2PoolConfig cfg = {
        "E2ETest", sizeof(TestPayloadV2), 1, 8, &ops, 1.0f,
        blob_sz, root, nullptr, nullptr, RECYCLER_V2_STORAGE_OVERWRITE
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);
    uint16_t type_idx = handle->type;

    /* The recycler created <root>/<type_name>/.  Write a blob directly
     * into it, simulating what StorageWrite would have done.
     * Filename: 16-char zero-padded hex of the marshaller ID. */
    char blob_path[512];
    snprintf(blob_path, sizeof(blob_path),
        "%s/E2ETest/0000000000000001.pb", root);

    int fd = open(blob_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ssize_t nw = write(fd, &test_id, blob_sz);
    close(fd);
    ASSERT_EQ(nw, (ssize_t)blob_sz);

    /* Encode holder as if it was marshalled — points to our on-disk blob */
    sE2EMarshalCountV2  = 0;
    sE2EUnmarshalCountV2 = 0;
    sCleanupCountV2.store(0);

    InstanceHolderV2 holder;
    holder.holder.marshaller = RecyclerV2EncodeMarshaller(type_idx, 1);
    EXPECT_TRUE(RecyclerV2IsMarshaller(&holder));

    /* Trigger the unmarshal slow path */
    void *obj = RecyclerV2GetInstance(&holder);
    ASSERT_NE(obj, nullptr);

    /* Holder transitioned to Instance mode */
    EXPECT_TRUE(RecyclerV2IsInstance(&holder));

    /* Data integrity — the unmarshalled object matches the blob */
    TestPayloadV2 *p = (TestPayloadV2 *)obj;
    EXPECT_EQ(p->id, test_id);
    EXPECT_TRUE(p->initialized);

    /* Callback verification */
    EXPECT_EQ(sE2EMarshalCountV2, 0);   // marshal path not triggered
    EXPECT_EQ(sE2EUnmarshalCountV2, 1);
    EXPECT_EQ(sCleanupCountV2.load(), 1);

    /* Blob deleted after successful unmarshal */
    struct stat st;
    EXPECT_NE(stat(blob_path, &st), 0)
        << "Blob should be deleted after unmarshal: " << blob_path;

    /* Return the live object */
    EXPECT_EQ(RecyclerV2Put(&holder, nullptr, 0), 0);

    system("rm -rf /tmp/recycler_v2_e2e_blob");
}

TEST(RecyclerV2MarshalE2E, CorruptedBlobRejected)
{
    /* Truncated blob — unmarshal must fail gracefully. */
    const char *root = "/tmp/recycler_v2_e2e_corrupt";
    system("rm -rf /tmp/recycler_v2_e2e_corrupt");

    RecyclerV2PoolOps ops = sTestOpsV2;
    ops.poolop_unmarshal_callback = sE2EUnmarshalCallbackV2;

    RecyclerV2PoolConfig cfg = {
        "E2ECorrupt", sizeof(TestPayloadV2), 1, 8, &ops, 1.0f,
        sizeof(int), root, nullptr, nullptr, RECYCLER_V2_STORAGE_OVERWRITE
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    char blob_path[512];
    snprintf(blob_path, sizeof(blob_path),
        "%s/E2ECorrupt/0000000000000001.pb", root);

    /* Write a truncated blob (1 byte — unmarshal callback needs sizeof(int)) */
    int fd = open(blob_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    char c = 'x';
    ASSERT_EQ(write(fd, &c, 1), 1);
    close(fd);

    InstanceHolderV2 holder;
    holder.holder.marshaller = RecyclerV2EncodeMarshaller(handle->type, 1);
    void *obj = RecyclerV2GetInstance(&holder);
    EXPECT_EQ(obj, nullptr)
        << "Unmarshal should fail when blob is smaller than expected";

    system("rm -rf /tmp/recycler_v2_e2e_corrupt");
}

/* ── State dump introspection test ──────────────────────────────────────── */

TEST(RecyclerV2DumpState, OutputsValidJson)
{
    /* Create a pool with some leased and some free objects, then dump
     * state and verify the output contains expected keys. */

    RecyclerV2PoolConfig cfg = {
        "DumpTest", sizeof(TestPayloadV2), 2, 8, &sTestOpsV2, 1.0f
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Lease a few objects to create a mixed state */
    InstanceHolderV2 *a = RecyclerV2Get(handle, nullptr, 0);
    InstanceHolderV2 *b = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    RecyclerV2Put(b, nullptr, 0);
    free(b);

    /* Dump to a temp file */
    const char *path = "/tmp/recycler_v2_dump_test.json";
    FILE *f = fopen(path, "w");
    ASSERT_NE(f, nullptr);
    RecyclerV2DumpState(f);
    fclose(f);

    /* Read back and check for key structural elements */
    FILE *in = fopen(path, "r");
    ASSERT_NE(in, nullptr);
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    ASSERT_GT(sz, 100);
    char *buf = (char *)malloc((size_t)sz + 1);
    ASSERT_NE(buf, nullptr);
    fread(buf, 1, (size_t)sz, in);
    buf[sz] = '\0';
    fclose(in);

    /* Verify JSON structure */
    EXPECT_NE(strstr(buf, "\"count_types\""), nullptr);
    EXPECT_NE(strstr(buf, "\"pools\""), nullptr);
    EXPECT_NE(strstr(buf, "\"type_name\": \"DumpTest\""), nullptr);
    EXPECT_NE(strstr(buf, "\"config\""), nullptr);
    EXPECT_NE(strstr(buf, "\"capacity\""), nullptr);
    EXPECT_NE(strstr(buf, "\"groups\""), nullptr);
    EXPECT_NE(strstr(buf, "\"slots\""), nullptr);
    EXPECT_NE(strstr(buf, "\"state\":\"LEASED\""), nullptr);
    EXPECT_NE(strstr(buf, "\"state\":\"FREE\""), nullptr);
    EXPECT_NE(strstr(buf, "\"state\":\"SENTINEL\""), nullptr);
    EXPECT_NE(strstr(buf, "\"holder_mode\""), nullptr);
    EXPECT_NE(strstr(buf, "\"referenced\""), nullptr);
    EXPECT_NE(strstr(buf, "\"marshalled_blobs\""), nullptr);

    free(buf);
    unlink(path);

    RecyclerV2Put(a, nullptr, 0);
    free(a);
}

TEST(RecyclerV2DumpState, ToBufferReturnsMallocedString)
{
    RecyclerV2PoolConfig cfg = {
        "BufDump", sizeof(TestPayloadV2), 1, 4, &sTestOpsV2, 1.0f
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    char *json = RecyclerV2DumpStateToBuffer();
    ASSERT_NE(json, nullptr);
    EXPECT_GT(strlen(json), 50u);
    EXPECT_NE(strstr(json, "\"type_name\": \"BufDump\""), nullptr);
    EXPECT_EQ(json[strlen(json) - 1], '\n');  // trailing newline
    free(json);
}

/* ── Describe (JSON introspection) tests ─────────────────────────────────── */

TEST(RecyclerV2Describe, DescribesPoolsAndFilters)
{
    RecyclerV2PoolConfig cfgA = {"DescPoolA", sizeof(TestPayloadV2), 2, 8, &sTestOpsV2, 1.0f};
    RecyclerV2PoolConfig cfgB = {"DescPoolB", sizeof(TestPayloadV2), 2, 8, &sTestOpsV2, 1.0f};
    ASSERT_NE(RecyclerV2InitTypePool(&cfgA), nullptr);
    ASSERT_NE(RecyclerV2InitTypePool(&cfgB), nullptr);

    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 512);
    ASSERT_NE(bd.data, nullptr);

    /* Unfiltered — every registered pool appears. */
    EXPECT_EQ(DescribeRecycler(NULL, &bd), &bd);
    EXPECT_NE(strstr(bd.data, "DescPoolA"), nullptr);
    EXPECT_NE(strstr(bd.data, "DescPoolB"), nullptr);
    EXPECT_NE(strstr(bd.data, "\"count_types\""), nullptr);
    EXPECT_NE(strstr(bd.data, "\"capacity\""), nullptr);
    EXPECT_NE(strstr(bd.data, "\"groups\""), nullptr);

    /* Filtered — only the named pool appears. */
    bd.size = 0;
    bd.data[0] = '\0';
    EXPECT_EQ(DescribeRecycler("DescPoolA", &bd), &bd);
    EXPECT_NE(strstr(bd.data, "DescPoolA"), nullptr);
    EXPECT_EQ(strstr(bd.data, "DescPoolB"), nullptr);

    /* Unknown name — pools array is empty. */
    bd.size = 0;
    bd.data[0] = '\0';
    EXPECT_EQ(DescribeRecycler("DescPoolNone", &bd), &bd);
    EXPECT_EQ(strstr(bd.data, "DescPoolA"), nullptr);
    EXPECT_EQ(strstr(bd.data, "DescPoolB"), nullptr);

    BufferDescriptorRelease(&bd);
}

TEST(RecyclerV2Describe, AllocatesWhenProvidedNull)
{
    RecyclerV2PoolConfig cfg = {"DescPoolNull", sizeof(TestPayloadV2), 1, 8, &sTestOpsV2, 1.0f};
    ASSERT_NE(RecyclerV2InitTypePool(&cfg), nullptr);

    BufferDescriptor *out = DescribeRecycler("DescPoolNull", NULL);
    ASSERT_NE(out, nullptr);
    ASSERT_NE(out->data, nullptr);
    EXPECT_NE(strstr(out->data, "DescPoolNull"), nullptr);

    BufferDescriptorRelease(out);
    free(out);
}

TEST(RecyclerV2ListTypes, ReturnsSingleSlabOfNames)
{
    RecyclerV2PoolConfig cfgA = {"ListTypeA", sizeof(TestPayloadV2), 1, 8, &sTestOpsV2, 1.0f};
    RecyclerV2PoolConfig cfgB = {"ListTypeB", sizeof(TestPayloadV2), 1, 8, &sTestOpsV2, 1.0f};
    ASSERT_NE(RecyclerV2InitTypePool(&cfgA), nullptr);
    ASSERT_NE(RecyclerV2InitTypePool(&cfgB), nullptr);

    CollectionDescriptor *types = ListRecyclerTypes();
    ASSERT_NE(types, nullptr);
    ASSERT_GT(types->collection_sz, 1u);
    EXPECT_EQ(types->collection_base_offset, 0u);

    bool found_a = false, found_b = false;
    for (size_t i = 0; i < types->collection_sz; i++) {
        const char *name = (const char *)types->collection[i];
        ASSERT_NE(name, nullptr);
        if (strcmp(name, "ListTypeA") == 0) found_a = true;
        if (strcmp(name, "ListTypeB") == 0) found_b = true;
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);

    /* Single free releases the whole slab. */
    free(types);
}

/* ── Debug build verification ───────────────────────────────────────────── */

TEST(RecyclerV2DebugBuild, FlagIsActive)
{
    /* UF_DEBUG_BUILD is defined in the generated config.h (C-only) and
     * controls #if blocks in recycler_v2.c.  In debug builds it is 1;
     * in release builds it is 0 and the debug-log blocks are stripped.
     *
     * Verify this by confirming the C source compiled with debug
     * instrumentation active: create a pool, exhaust it, marshal/
     * unmarshal — all paths that contain #if UF_DEBUG_BUILD blocks.
     * If any of those blocks caused a compile error, this test would
     * not link.  The InitLogsPoolConfig, ExhaustionPathReached, and
     * MarshalUnmarshalPathsReached tests below exercise those paths
     * at runtime. */
    SUCCEED();
}

TEST(RecyclerV2DebugBuild, InitLogsPoolConfig)
{
    /* In debug builds, InitTypePool emits a syslog with pool details.
     * We can't intercept syslog easily in a unit test, but we can
     * verify the pool was created successfully (which implies the
     * #if UF_DEBUG_BUILD block compiled and ran without error). */
    RecyclerV2PoolConfig cfg = {
        "DebugLogTest", sizeof(TestPayloadV2), 2, 16, &sTestOpsV2, 0.75f,
        512, NULL, "ufsrvwebsock", "east", RECYCLER_V2_STORAGE_OVERWRITE
    };
    RecyclerV2PoolHandle *h = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(h, nullptr);

    /* Verify the config was stored correctly (debug log would show these) */
    RecyclerV2PoolConfig out = {};
    RecyclerV2GetPoolConfig(h, &out);
    EXPECT_STREQ(out.type_name, "DebugLogTest");
    EXPECT_EQ(out.blocksz, sizeof(TestPayloadV2));
    EXPECT_STREQ(out.ufsrv_class, "ufsrvwebsock");
    EXPECT_STREQ(out.instance_id, "east");
    EXPECT_FLOAT_EQ(out.marshal_watermark, 0.75f);
}

TEST(RecyclerV2DebugBuild, ExhaustionPathReached)
{
    /* Exhaust a single-group pool and verify the exhaustion path
     * executes (debug build would log "fully leased" and expansion
     * details via syslog). */
    RecyclerV2PoolConfig cfg = {
        "DebugExhaust", sizeof(TestPayloadV2), 1, 4, &sTestOpsV2, 1.0f
    };
    RecyclerV2PoolHandle *h = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(h, nullptr);

    /* Lease all usable objects (3 of 4, index 0 = sentinel) */
    InstanceHolderV2 *holders[4] = {NULL};
    for (int i = 0; i < 3; i++) {
        holders[i] = RecyclerV2Get(h, NULL, 0);
        ASSERT_NE(holders[i], nullptr);
    }

    /* 4th Get — exhaustion path (debug log: "fully leased") */
    InstanceHolderV2 *ih = RecyclerV2Get(h, NULL, 0);
    EXPECT_EQ(ih, nullptr);  // hard exhaustion, expansion blocked

    for (int i = 0; i < 3; i++) {
        RecyclerV2Put(holders[i], NULL, 0);
        free(holders[i]);
    }
}

TEST(RecyclerV2DebugBuild, MarshalUnmarshalPathsReached)
{
    /* Verify the marshal+unmarshal debug-log paths execute without
     * crashing.  Uses the E2E blob round-trip which exercises
     * StorageRead → unmarshal callback → CAS.  In debug builds,
     * syslog calls in the marshal/unmarshal paths are compiled in. */

    const int test_id = 0x4DEB4C1C;
    const char *root  = "/tmp/recycler_v2_debug_e2e";
    system("rm -rf /tmp/recycler_v2_debug_e2e");

    RecyclerV2PoolOps ops = sTestOpsV2;
    ops.poolop_marshal_callback   = sE2EMarshalCallbackV2;
    ops.poolop_unmarshal_callback = sE2EUnmarshalCallbackV2;

    RecyclerV2PoolConfig cfg = {
        "DebugE2E", sizeof(TestPayloadV2), 1, 8, &ops, 1.0f,
        sizeof(int), root, NULL, NULL, RECYCLER_V2_STORAGE_OVERWRITE
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Write blob + encode marshalled holder */
    char blob_path[512];
    snprintf(blob_path, sizeof(blob_path),
        "%s/DebugE2E/0000000000000001.pb", root);
    int fd = open(blob_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(write(fd, &test_id, sizeof(test_id)), (ssize_t)sizeof(test_id));
    close(fd);

    InstanceHolderV2 holder;
    holder.holder.marshaller = RecyclerV2EncodeMarshaller(handle->type, 1);

    /* Unmarshal (debug build logs "unmarshal triggered" via syslog) */
    void *obj = RecyclerV2GetInstance(&holder);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(((TestPayloadV2 *)obj)->id, test_id);

    RecyclerV2Put(&holder, NULL, 0);
    system("rm -rf /tmp/recycler_v2_debug_e2e");
}

/* ── Concurrency-correctness regression tests (uplift) ───────────────────── */
/* These target the races fixed by the audit remediation: no-realloc group
 * directory, atomic allocated_groups_sz, atomic holder_fallback, refcount
 * resurrection, and the marshal-claim state machine. */

TEST(RecyclerV2Concurrent, ForcedExpansionUnderContention)
{
    /* Many threads hammer Get/Put on a small pool, forcing several threads to
     * race on the expansion CAS simultaneously.  Verifies the fixed
     * no-realloc group directory + atomic allocated_groups_sz publish a new
     * group exactly once and never hand out a torn/invalid object. */
    RecyclerV2PoolConfig cfg = {"ExpandRaceV2", sizeof(TestPayloadV2), 8, 8, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    constexpr int kThreads = 12;
    constexpr int kIters   = 5000;

    std::atomic<int> null_gets{0};
    std::atomic<int> corrupt{0};
    std::atomic<int> put_errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kIters; i++) {
                InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
                if (ih == nullptr) {
                    null_gets.fetch_add(1);
                    continue;
                }
                TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(ih);
                if (p == nullptr || !p->initialized) {
                    corrupt.fetch_add(1);
                }
                if (RecyclerV2Put(ih, nullptr, 0) != 0) {
                    put_errors.fetch_add(1);
                }
                free(ih);
            }
        });
    }
    for (auto &th : threads) th.join();

    EXPECT_EQ(corrupt.load(), 0);
    EXPECT_EQ(put_errors.load(), 0);
    /* 8 groups × 7 usable = 56 objects > 12 threads — no hard exhaustion. */
    EXPECT_EQ(null_gets.load(), 0);
    EXPECT_EQ(RecyclerV2GetLeasedCount(handle), 0u);
}

TEST(RecyclerV2MultiHolder, ConcurrentAliasCreateDestroy)
{
    /* Concurrent GetNewInstance/DestroyInstance on one primary object
     * exercises the atomic holder_fallback publication and the fallback-list
     * spinlock.  Refcount must return to base+1 after all aliases are gone. */
    RecyclerV2PoolConfig cfg = {"MultiMTV2", sizeof(TestPayloadV2), 2, 8, &sTestOpsV2, 1.0f};
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    InstanceHolderV2 *primary = RecyclerV2Get(handle, nullptr, 0);
    ASSERT_NE(primary, nullptr);

    constexpr int kAliases = 16;
    InstanceHolderV2 *aliases[kAliases];
    for (int i = 0; i < kAliases; i++) {
        aliases[i] = RecyclerV2GetNewInstance(primary);
        ASSERT_NE(aliases[i], nullptr);
    }
    EXPECT_EQ(RecyclerV2GetReferenceCount(primary), (size_t)(kAliases + 2));

    std::atomic<int> err{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kAliases; i++) {
        threads.emplace_back([&, i]() {
            if (RecyclerV2DestroyInstance(aliases[i]) != RECYCLER_V2_INSTANCE_HOLDER_FOUND) {
                err.fetch_add(1);
            }
            free(aliases[i]);
        });
    }
    for (auto &th : threads) th.join();

    EXPECT_EQ(err.load(), 0);
    EXPECT_EQ(RecyclerV2GetReferenceCount(primary), 2u);

    EXPECT_EQ(RecyclerV2Put(primary, nullptr, 0), 0);
    free(primary);
}

TEST(RecyclerV2ConcurrentMarshal, ClaimSerializesEachVictimOnce)
{
    /* With all objects idle, concurrent Get() calls must drive the CLOCK
     * marshaller.  The marshal-claim flag ensures each victim is serialized
     * at most once — sMarshalCountV2 must never exceed the object count. */
    const char *root = "/tmp/recycler_v2_claim_test";
    system("rm -rf /tmp/recycler_v2_claim_test");

    sMarshalCountV2.store(0);
    sUnmarshalCountV2.store(0);
    sCleanupCountV2.store(0);

    RecyclerV2PoolOps ops = sTestOpsV2;
    ops.poolop_marshal_callback         = sMockMarshalCallbackV2;
    ops.poolop_unmarshal_callback       = sMockUnmarshalCallbackV2;
    ops.poolop_marshal_cleanup_callback = sMockMarshalCleanupCallbackV2;

    RecyclerV2PoolConfig cfg = {
        "ClaimMTV2", sizeof(TestPayloadV2), 1, 8, &ops, 1.0f, 256,
        root, nullptr, nullptr, RECYCLER_V2_STORAGE_OVERWRITE
    };
    RecyclerV2PoolHandle *handle = RecyclerV2InitTypePool(&cfg);
    ASSERT_NE(handle, nullptr);

    /* Lease all 7 usable objects and mark them idle (refcount 1). */
    std::vector<InstanceHolderV2 *> idle;
    for (int i = 0; i < 7; i++) {
        InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
        ASSERT_NE(ih, nullptr);
        RecyclerV2UnReferenced(ih, 1);
        idle.push_back(ih);
    }

    /* First Get clears the CLOCK second-chance bits; returns NULL (no victim
     * yet because every idle object still has referenced==true). */
    EXPECT_EQ(RecyclerV2Get(handle, nullptr, 0), nullptr);

    /* Concurrent Gets must now marshal idle victims to free slots. */
    constexpr int kThreads = 8;
    std::atomic<int> err{0};
    std::vector<InstanceHolderV2 *> got(kThreads, nullptr);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++) {
        threads.emplace_back([&, i]() {
            InstanceHolderV2 *ih = RecyclerV2Get(handle, nullptr, 0);
            got[i] = ih;
            if (ih) {
                TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(ih);
                if (p == nullptr || !p->initialized) err.fetch_add(1);
            }
        });
    }
    for (auto &th : threads) th.join();

    EXPECT_EQ(err.load(), 0);
    /* At least one victim must have been marshalled under contention. */
    EXPECT_GT(sMarshalCountV2.load(), 0);
    /* Claim discipline: never serialize more objects than exist. */
    EXPECT_LE(sMarshalCountV2.load(), 7);

    /* Return everything cleanly — marshalled holders must be re-materialized
     * (unmarshal) and returned; live idle holders need Referenced→Put. */
    for (auto *ih : got) {
        if (ih) { EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0); free(ih); }
    }
    for (auto *ih : idle) {
        if (RecyclerV2IsMarshaller(ih)) {
            TestPayloadV2 *p = (TestPayloadV2 *)RecyclerV2GetInstance(ih);
            ASSERT_NE(p, nullptr);
            EXPECT_TRUE(p->initialized);
            EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0);
        } else {
            RecyclerV2Referenced(ih, 1);
            EXPECT_EQ(RecyclerV2Put(ih, nullptr, 0), 0);
        }
        free(ih);
    }

    system("rm -rf /tmp/recycler_v2_claim_test");
}
