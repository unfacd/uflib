/**
 * @file file_loader_concurrent_access_tests.cpp
 * @brief FileLoaderService — gtest unit tests.
 *
 * Copyright (C) 2015-2026 unfacd works
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <unistd.h>

extern "C" {
#include <uflib/file_loader_service_concurrent/file_loader_concurrent_service.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/logger/logger.h>
}

static std::string sTmpPath(const char *name) {
    return std::string("/tmp/fls_test_") + name;
}
static void sCreateFile(const char *path, const char *content) {
    FILE *f = fopen(path, "w"); assert(f); fputs(content, f); fclose(f);
}
static void sCleanup(FileLoaderService *svc, pthread_t *th) {
    if (svc) { FileLoaderServiceStop(svc); if (th) *th = (pthread_t)0;
        FileLoaderServiceDestroy(svc); }
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

TEST(FileLoaderServiceTest, InitNullConfig)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    EXPECT_NE(th, (pthread_t)0);
    sCleanup(flc.service_handle, &th);
}

TEST(FileLoaderServiceTest, InitWithConfig)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 64;
    flc.service_config_params.loaded_size   = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    EXPECT_EQ(FileLoaderServiceCapacity(flc.service_handle), 64u);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
}

TEST(FileLoaderServiceTest, DestroyNullSafe)
{ FileLoaderServiceDestroy(nullptr); SUCCEED(); }

TEST(FileLoaderServiceTest, DoubleDestroy)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    EXPECT_NE(th, (pthread_t)0);
    FileLoaderServiceStop(flc.service_handle);
    FileLoaderServiceDestroy(flc.service_handle);
    flc.service_handle = NULL;
    FileLoaderServiceDestroy(flc.service_handle);  /* NULL-safe */
    SUCCEED();
}

TEST(FileLoaderServiceTest, StopReapsTheMonitorThread)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    ASSERT_NE(FileLoaderServiceGetThread(flc.service_handle), (pthread_t)0);
    FileLoaderServiceStop(flc.service_handle);
    EXPECT_EQ(FileLoaderServiceGetThread(flc.service_handle), (pthread_t)0);
    FileLoaderServiceStop(flc.service_handle);
    EXPECT_EQ(FileLoaderServiceGetThread(flc.service_handle), (pthread_t)0);
    FileLoaderServiceDestroy(flc.service_handle);
}

TEST(FileLoaderServiceTest, DestroyWithoutStopReapsTheMonitorThread)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    ASSERT_NE(FileLoaderServiceGetThread(flc.service_handle), (pthread_t)0);
    FileLoaderServiceDestroy(flc.service_handle);
    SUCCEED();
}

/* ==========================================================================
 * AddFile / GetFileInfo / RemoveFile
 * ========================================================================== */

TEST(FileLoaderServiceTest, AddFileReturnsHandle)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    std::string path = sTmpPath("addfile");
    sCreateFile(path.c_str(), "hello");
    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    EXPECT_NE(h, (FileLoaderServiceFileHandle)0);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink(path.c_str());
}

TEST(FileLoaderServiceTest, AddFileBlacklisted)
{
    const char *bl[] = { "/tmp/fls_blacklisted", nullptr };
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.blacklist_entries      = bl;
    flc.service_config_params.blacklist_entry_count = 1;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    sCreateFile("/tmp/fls_blacklisted", "secret");
    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, "/tmp/fls_blacklisted");
    EXPECT_EQ(h, (FileLoaderServiceFileHandle)0);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink("/tmp/fls_blacklisted");
}

TEST(FileLoaderServiceTest, GetFileInfo)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    std::string path = sTmpPath("getinfo");
    sCreateFile(path.c_str(), "test");
    FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    FileLoaderServiceFileInfo info;
    EXPECT_TRUE(FileLoaderServiceGetFileInfo(flc.service_handle, path.c_str(), &info));
    EXPECT_TRUE(info.in_registry);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink(path.c_str());
}

TEST(FileLoaderServiceTest, RemoveFile)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    std::string path = sTmpPath("remove");
    sCreateFile(path.c_str(), "gone");
    FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    FileLoaderServiceRemoveFile(flc.service_handle, path.c_str());
    FileLoaderServiceFileInfo info;
    FileLoaderServiceGetFileInfo(flc.service_handle, path.c_str(), &info);
    EXPECT_FALSE(info.in_registry);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink(path.c_str());
}

/* ==========================================================================
 * Query
 * ========================================================================== */

TEST(FileLoaderServiceTest, SizeAndCapacity)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 128;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    EXPECT_EQ(FileLoaderServiceCapacity(flc.service_handle), 128u);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
}

TEST(FileLoaderServiceTest, Heartbeat)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    usleep(600000);
    EXPECT_GT(FileLoaderServiceHeartbeat(flc.service_handle), (uint64_t)0);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
}

TEST(FileLoaderServiceTest, RegisterUnregisterThread)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    EXPECT_TRUE(FileLoaderServiceRegisterThread(flc.service_handle));
    FileLoaderServiceUnregisterThread(flc.service_handle);
    FileLoaderServiceUnregisterThread(flc.service_handle);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
}

TEST(FileLoaderServiceTest, AddFileNullPath)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    EXPECT_EQ(FileLoaderServiceAddFile(flc.service_handle, nullptr), (FileLoaderServiceFileHandle)0);
    EXPECT_EQ(FileLoaderServiceAddFile(flc.service_handle, ""), (FileLoaderServiceFileHandle)0);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
}

/* ==========================================================================
 * Pool exhaustion (Tier 1 — registry slot)
 * ========================================================================== */

TEST(FileLoaderServiceTest, PoolExhaustionNoEviction)
{
    // Small pool, no eviction callback → AddFile returns 0 when full.
    // This exercises the sNodeAllocate → returns 0 → AddFile returns 0 path.
    // registry_size counts usable nodes; the sentinel node is allocated on top.
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;  // 8 usable nodes
    flc.service_config_params.loaded_size   = 4;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::vector<std::string> paths;
    char nameBuf[64];
    // Fill all 8 usable nodes
    for (int i = 0; i < 8; i++) {
        snprintf(nameBuf, sizeof(nameBuf), "pool_%d", i);
        std::string p = sTmpPath(nameBuf);
        sCreateFile(p.c_str(), "x");
        paths.push_back(p);
        FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, p.c_str());
        ASSERT_NE(h, (FileLoaderServiceFileHandle)0) << "failed to add file " << i;
    }

    // 9th file — pool is full, no eviction callback → returns 0
    std::string extra = sTmpPath("pool_extra");
    sCreateFile(extra.c_str(), "overflow");
    FileLoaderServiceFileHandle h9 = FileLoaderServiceAddFile(flc.service_handle, extra.c_str());
    EXPECT_EQ(h9, (FileLoaderServiceFileHandle)0) << "pool should be exhausted";

    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    for (auto &p : paths) unlink(p.c_str());
    unlink(extra.c_str());
}

TEST(FileLoaderServiceTest, PoolReuseAfterRemove)
{
    // Remove one file → free pool slot → re-add succeeds.
    // registry_size counts usable nodes; the sentinel node is allocated on top.
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 4;  // 4 usable nodes
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::vector<std::string> paths;
    char nameBuf[64];
    // Fill all 4 usable nodes
    for (int i = 0; i < 4; i++) {
        snprintf(nameBuf, sizeof(nameBuf), "reuse_%d", i);
        std::string p = sTmpPath(nameBuf);
        sCreateFile(p.c_str(), "x");
        paths.push_back(p);
        ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, p.c_str()),
                  (FileLoaderServiceFileHandle)0);
    }

    // Pool full
    std::string extra = sTmpPath("reuse_extra");
    sCreateFile(extra.c_str(), "z");
    EXPECT_EQ(FileLoaderServiceAddFile(flc.service_handle, extra.c_str()),
              (FileLoaderServiceFileHandle)0);

    // Remove one file → free a slot
    FileLoaderServiceRemoveFile(flc.service_handle, paths[0].c_str());

    // Re-add the extra file — should succeed (slot was freed)
    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, extra.c_str());
    EXPECT_NE(h, (FileLoaderServiceFileHandle)0);

    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    for (auto &p : paths) unlink(p.c_str());
    unlink(extra.c_str());
}

/* ==========================================================================
 * Buffer cache eviction (Tier 2 — loaded buffer)
 * ========================================================================== */

TEST(FileLoaderServiceTest, BufferCacheEviction)
{
    // loaded_size (4) < registry_size (16 usable nodes) — adding 15 files
    // forces the buffer cache to evict cold entries.  After all files are
    // added and loaded, every file must still be registered (in_registry=true).
    // This exercises sClockEvictBuffer → sUnloadBuffer.
    // registry_size counts usable nodes; the sentinel node is allocated on top.
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 16;  // 16 usable nodes
    flc.service_config_params.loaded_size   = 4;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    const int N = 15;
    std::vector<std::string> paths;
    char nameBuf[64], contentBuf[64];
    for (int i = 0; i < N; i++) {
        snprintf(nameBuf, sizeof(nameBuf), "cache_%d", i);
        snprintf(contentBuf, sizeof(contentBuf), "cache file %d", i);
        std::string p = sTmpPath(nameBuf);
        sCreateFile(p.c_str(), contentBuf);
        paths.push_back(p);
        FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, p.c_str());
        ASSERT_NE(h, (FileLoaderServiceFileHandle)0) << "add failed for file " << i;
    }

    // Give the monitor time to load files (500ms epoll timeout × a few cycles)
    usleep(2000000);

    // All N files must still be registered even though buffer cache only holds 4
    for (int i = 0; i < N; i++) {
        FileLoaderServiceFileInfo info;
        EXPECT_TRUE(FileLoaderServiceGetFileInfo(flc.service_handle,
            paths[i].c_str(), &info)) << "file " << i << " not in registry";
        EXPECT_TRUE(info.in_registry) << "file " << i << " in_registry should be true";
    }

    EXPECT_EQ(FileLoaderServiceSize(flc.service_handle), (uint32_t)N);

    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    for (auto &p : paths) unlink(p.c_str());
}

static std::string sDescribe(FileLoaderService *svc);
static size_t      sCount(const std::string &hay, const char *needle);
static int         sMappedFileCount(const char *needle);
static void        sStopAndDestroy(FileLoaderService *svc);

TEST(FileLoaderServiceTest, EvictionReleasesUncachedMappings)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    flc.service_config_params.loaded_size   = 2;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    const int N = 6;
    char nameBuf[64], contentBuf[64];
    for (int i = 0; i < N; i++) {
        snprintf(nameBuf, sizeof(nameBuf), "evictmap_%d", i);
        snprintf(contentBuf, sizeof(contentBuf), "evict payload %d", i);
        std::string p = sTmpPath(nameBuf);
        sCreateFile(p.c_str(), contentBuf);
        ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, p.c_str()),
                  (FileLoaderServiceFileHandle)0) << "add failed for " << i;
    }
    usleep(3000000);

    std::string js = sDescribe(flc.service_handle);
    ASSERT_FALSE(js.empty());
    size_t loaded_true = sCount(js, "\"loaded\":true");
    size_t cached_true = sCount(js, "\"in_cache\":true");

    EXPECT_LE(cached_true, 2u) << "cache grew past its capacity\n" << js;
    EXPECT_EQ(loaded_true, cached_true)
        << "a mapping is published for a file that holds no cache slot, so "
           "eviction never ran\n" << js;
    EXPECT_LE(sMappedFileCount("/tmp/fls_test_evictmap_"), 2)
        << "more mappings alive than cache slots: eviction did not unmap";

    sStopAndDestroy(flc.service_handle);
    for (int i = 0; i < N; i++) { snprintf(nameBuf, sizeof(nameBuf), "evictmap_%d", i);
        unlink(sTmpPath(nameBuf).c_str()); }
}

/* ==========================================================================
 * GetFileContent / ReturnFile round-trip
 * ========================================================================== */

TEST(FileLoaderServiceTest, GetFileContentRoundTrip)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 16;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    // KNOWN API GAP (TD-013): AddFile returns a handle that becomes stale
    // after the monitor loads the file (sLoadFile bumps node generation).
    // There is no public API to obtain a valid handle for an
    // already-registered path — AddFile on an existing path returns 0.
    // The consumer needs a "RefreshHandle" or "GetHandleForPath" primitive.
    //
    // We verify: the file can be registered, the monitor starts, and
    // GetFileInfo returns correct registry state.
    std::string path = sTmpPath("roundtrip");
    sCreateFile(path.c_str(), "round-trip data payload");
    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h, (FileLoaderServiceFileHandle)0);

    usleep(600000);  // One epoll cycle for the monitor

    FileLoaderServiceFileInfo info;
    ASSERT_TRUE(FileLoaderServiceGetFileInfo(flc.service_handle,
        path.c_str(), &info));
    EXPECT_TRUE(info.in_registry);
    EXPECT_GT(FileLoaderServiceHeartbeat(flc.service_handle), (uint64_t)0);

    FileLoaderServiceUnregisterThread(flc.service_handle);

    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink(path.c_str());
}

/* ==========================================================================
 * Stale handle detection
 * ========================================================================== */

TEST(FileLoaderServiceTest, StaleHandleAfterSlotReuseReturnsEstale)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    std::string a = sTmpPath("reuse_a");
    std::string b = sTmpPath("reuse_b");
    sCreateFile(a.c_str(), "AAAAAAAAAAAAAAAA");
    sCreateFile(b.c_str(), "BBBB");

    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, a.c_str());
    ASSERT_NE(h, (FileLoaderServiceFileHandle)0);
    FileLoaderServiceRemoveFile(flc.service_handle, a.c_str());
    FileLoaderServiceFileHandle hb = FileLoaderServiceAddFile(flc.service_handle, b.c_str());
    ASSERT_NE(hb, (FileLoaderServiceFileHandle)0);
    ASSERT_EQ(h >> 44, hb >> 44) << "the slot was not recycled, so this test would prove nothing";

    size_t sz = 0;
    errno = 0;
    void *data = FileLoaderServiceGetFileContent(flc.service_handle, h, &sz);
    if (data) {
        ADD_FAILURE() << "stale handle served " << std::string((const char *)data, sz) << " from the slot's new occupant";
        FileLoaderServiceReturnFile(flc.service_handle, data);
    }
    EXPECT_EQ(data, nullptr);
    EXPECT_EQ(errno, ESTALE);

    size_t szb = 0;
    void  *db  = FileLoaderServiceGetFileContent(flc.service_handle, hb, &szb);
    for (int i = 0; i < 200 && !db; i++) {
        usleep(10000);
        db = FileLoaderServiceGetFileContent(flc.service_handle, hb, &szb);
    }
    ASSERT_NE(db, nullptr) << "the new occupant never loaded, so the slot-reuse precondition is unproven";
    EXPECT_EQ(std::string((const char *)db, szb), "BBBB");
    FileLoaderServiceReturnFile(flc.service_handle, db);

    FileLoaderServiceUnregisterThread(flc.service_handle);
    sStopAndDestroy(flc.service_handle);
    unlink(a.c_str());
    unlink(b.c_str());
}

TEST(FileLoaderServiceTest, StaleHandleAfterRemove)
{
    // AddFile → capture handle → RemoveFile → GetFileContent on old handle
    // → must fail with NULL and errno == ESTALE.
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    std::string path = sTmpPath("stale");
    sCreateFile(path.c_str(), "will be removed");
    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h, (FileLoaderServiceFileHandle)0);

    FileLoaderServiceRemoveFile(flc.service_handle, path.c_str());

    size_t sz = 0;
    errno = 0;
    void *data = FileLoaderServiceGetFileContent(flc.service_handle, h, &sz);
    EXPECT_EQ(data, nullptr);
    EXPECT_EQ(errno, ESTALE);

    FileLoaderServiceUnregisterThread(flc.service_handle);
    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink(path.c_str());
}

/* ==========================================================================
 * Re-add after remove (generation preservation)
 * ========================================================================== */

TEST(FileLoaderServiceTest, ReAddAfterRemove)
{
    // RemoveFile bumps generation.  Re-AddFile on the same path must produce
    // a new handle.  This exercises generation preservation across recycles
    // (design refinement §15.3).
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string path = sTmpPath("readd");
    sCreateFile(path.c_str(), "v1");
    FileLoaderServiceFileHandle h1 = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h1, (FileLoaderServiceFileHandle)0);

    FileLoaderServiceRemoveFile(flc.service_handle, path.c_str());

    // Re-create with different content
    unlink(path.c_str());
    sCreateFile(path.c_str(), "v2");
    FileLoaderServiceFileHandle h2 = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h2, (FileLoaderServiceFileHandle)0);

    // Handle must differ (generation bump)
    EXPECT_NE(h1, h2);

    // A handle identifies one add-incarnation.  Remove+re-add is a new
    // incarnation, so the old handle is stale and must not be recovered on
    // the strength of the path it once named.
    FileLoaderServiceRegisterThread(flc.service_handle);
    size_t sz = 0;
    errno = 0;
    void *data = FileLoaderServiceGetFileContent(flc.service_handle, h1, &sz);
    EXPECT_EQ(data, nullptr) << "stale handle was recovered instead of rejected";
    EXPECT_EQ(errno, ESTALE);

    size_t sz2 = 0;
    void  *data2 = FileLoaderServiceGetFileContent(flc.service_handle, h2, &sz2);
    for (int i = 0; i < 200 && !data2; i++) {
        usleep(10000);
        data2 = FileLoaderServiceGetFileContent(flc.service_handle, h2, &sz2);
    }
    ASSERT_NE(data2, nullptr) << "the re-added file never loaded";
    EXPECT_EQ(std::string((const char *)data2, sz2), "v2");
    FileLoaderServiceReturnFile(flc.service_handle, data2);
    FileLoaderServiceUnregisterThread(flc.service_handle);

    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink(path.c_str());
}

/* ==========================================================================
 * Duplicate add
 * ========================================================================== */

TEST(FileLoaderServiceTest, ReusedSlotNeverServesThePreviousOccupant)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 16;
    flc.service_config_params.loaded_size   = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    std::string a     = sTmpPath("reuseocc_a");
    std::string b     = sTmpPath("reuseocc_b");
    std::string drain = sTmpPath("reuseocc_drain");
    sCreateFile(a.c_str(), "AAAAAAAAAAAAAAAAAAAAAAAA");
    sCreateFile(b.c_str(), "BBBBBBBB");
    sCreateFile(drain.c_str(), "drain");

    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, drain.c_str()), (FileLoaderServiceFileHandle)0);
    FileLoaderServiceRemoveFile(flc.service_handle, drain.c_str());

    for (int i = 0; i < 5000; i++) {
        FileLoaderServiceFileHandle ha = FileLoaderServiceAddFile(flc.service_handle, a.c_str());
        ASSERT_NE(ha, (FileLoaderServiceFileHandle)0) << "iteration " << i;
        FileLoaderServiceRemoveFile(flc.service_handle, a.c_str());

        FileLoaderServiceFileHandle hb = FileLoaderServiceAddFile(flc.service_handle, b.c_str());
        ASSERT_NE(hb, (FileLoaderServiceFileHandle)0) << "iteration " << i;
        size_t sz = 0;
        void  *d  = FileLoaderServiceGetFileContent(flc.service_handle, hb, &sz);
        ASSERT_NE(d, nullptr) << "iteration " << i;
        ASSERT_EQ(std::string((const char *)d, sz), "BBBBBBBB")
            << "iteration " << i << " size=" << sz << " expected=8 bytes='" << std::string((const char *)d, sz) << "'";
        FileLoaderServiceReturnFile(flc.service_handle, d);
        FileLoaderServiceRemoveFile(flc.service_handle, b.c_str());
    }

    FileLoaderServiceUnregisterThread(flc.service_handle);
    sStopAndDestroy(flc.service_handle);
    unlink(a.c_str());
    unlink(b.c_str());
    unlink(drain.c_str());
}

TEST(FileLoaderServiceTest, DuplicateAddDoesNotPublishALoadableNode)
{
    const char *needle = "/tmp/fls_test_dupwin_";
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 16;
    flc.service_config_params.loaded_size   = 4;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    std::string keep   = sTmpPath("dupwin_keep");
    std::string victim = sTmpPath("dupwin_victim");
    std::string dup    = sTmpPath("dupwin_dup");
    sCreateFile(keep.c_str(), "keep");
    sCreateFile(victim.c_str(), "victim");
    sCreateFile(dup.c_str(), "duplicate");

    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, keep.c_str()), (FileLoaderServiceFileHandle)0);
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, victim.c_str()), (FileLoaderServiceFileHandle)0);
    FileLoaderServiceRemoveFile(flc.service_handle, victim.c_str());

    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, dup.c_str());
    ASSERT_NE(h, (FileLoaderServiceFileHandle)0);
    for (int i = 0; i < 200000; i++) {
        FileLoaderServiceFileHandle again = FileLoaderServiceAddFile(flc.service_handle, dup.c_str());
        ASSERT_EQ(again, h) << "duplicate add changed the handle at iteration " << i;
    }

    FileLoaderServiceUnregisterThread(flc.service_handle);
    sStopAndDestroy(flc.service_handle);

    EXPECT_EQ(sMappedFileCount(needle), 0) << "a duplicate add left a mapping behind";
    unlink(keep.c_str());
    unlink(victim.c_str());
    unlink(dup.c_str());
}

TEST(FileLoaderServiceTest, DuplicateAddSamePath)
{
    // Adding the same path twice: first succeeds, second returns 0
    // (hashmap rejects duplicate key).  The file is still registered.
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string path = sTmpPath("dup");
    sCreateFile(path.c_str(), "same");
    FileLoaderServiceFileHandle h1 = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h1, (FileLoaderServiceFileHandle)0);

    // Duplicate add now returns a valid handle (AddFile is idempotent —
    // hashmap lookup recovers the existing mapping).
    FileLoaderServiceFileHandle h2 = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    EXPECT_NE(h2, (FileLoaderServiceFileHandle)0);
    // Both handles point to the same file — size is still 1
    FileLoaderServiceFileInfo info;
    EXPECT_TRUE(FileLoaderServiceGetFileInfo(flc.service_handle, path.c_str(), &info));
    EXPECT_TRUE(info.in_registry);
    EXPECT_EQ(FileLoaderServiceSize(flc.service_handle), 1u);

    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink(path.c_str());
}

/* ==========================================================================
 * Describe — JSON introspection
 * ========================================================================== */

struct JsonScan {
    const char *s;
    size_t      n;
    size_t      i;

    void ws() {
        while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) i++;
    }
    bool literal(const char *lit) {
        size_t len = strlen(lit);
        if (i + len > n || memcmp(s + i, lit, len) != 0) return false;
        i += len;
        return true;
    }
    bool str() {
        if (i >= n || s[i] != '"') return false;
        i++;
        while (i < n) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"') { i++; return true; }
            if (c < 0x20) return false;
            if (c != '\\') { i++; continue; }
            i++;
            if (i >= n) return false;
            char e = s[i];
            if (e == 'u') {
                if (i + 4 >= n) return false;
                for (int k = 1; k <= 4; k++)
                    if (!isxdigit((unsigned char)s[i + (size_t)k])) return false;
                i += 5;
            } else if (strchr("\"\\/bfnrt", e) != nullptr) {
                i++;
            } else {
                return false;
            }
        }
        return false;
    }
    bool value() {
        ws();
        if (i >= n) return false;
        if (s[i] == '"') return str();
        if (s[i] == '{') {
            i++; ws();
            if (i < n && s[i] == '}') { i++; return true; }
            while (true) {
                ws();
                if (!str()) return false;
                ws();
                if (i >= n || s[i] != ':') return false;
                i++;
                if (!value()) return false;
                ws();
                if (i >= n) return false;
                if (s[i] == ',') { i++; continue; }
                if (s[i] == '}') { i++; return true; }
                return false;
            }
        }
        if (s[i] == '[') {
            i++; ws();
            if (i < n && s[i] == ']') { i++; return true; }
            while (true) {
                if (!value()) return false;
                ws();
                if (i >= n) return false;
                if (s[i] == ',') { i++; continue; }
                if (s[i] == ']') { i++; return true; }
                return false;
            }
        }
        if (literal("true") || literal("false") || literal("null")) return true;
        size_t start = i;
        if (i < n && s[i] == '-') i++;
        while (i < n && isdigit((unsigned char)s[i])) i++;
        if (i < n && s[i] == '.') { i++; while (i < n && isdigit((unsigned char)s[i])) i++; }
        if (i < n && (s[i] == 'e' || s[i] == 'E')) {
            i++;
            if (i < n && (s[i] == '+' || s[i] == '-')) i++;
            while (i < n && isdigit((unsigned char)s[i])) i++;
        }
        return i > start;
    }
};

static bool sJsonWellFormed(const char *s, size_t n) {
    if (!s) return false;
    JsonScan js{s, n, 0};
    if (!js.value()) return false;
    js.ws();
    return js.i == n;
}

static std::string sDescribe(FileLoaderService *svc) {
    BufferDescriptor *bd = FileLoaderServiceDescribe(svc, nullptr);
    std::string       out;
    if (!bd) return out;
    if (bd->data) out.assign(bd->data, bd->size);
    BufferDescriptorRelease(bd);
    free(bd);
    return out;
}

static bool sContains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

static size_t sCount(const std::string &hay, const char *needle) {
    size_t n = 0, pos = 0, len = strlen(needle);
    while ((pos = hay.find(needle, pos)) != std::string::npos) { n++; pos += len; }
    return n;
}

static int sMappedFileCount(const char *needle) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    int  n = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

static UfLogger *sMakeLogger() {
    UfLogger *logger = nullptr;
    if (UfLoggerCreateWithDefaults(&logger) != UF_LOGGER_STATUS_OK) return nullptr;
    return logger;
}

static void sStopAndDestroy(FileLoaderService *svc) {
    if (!svc) return;
    FileLoaderServiceStop(svc);
    FileLoaderServiceDestroy(svc);
}

TEST(FileLoaderServiceDescribeTest, NullHandleIsReportedNotCrashed)
{
    BufferDescriptor *bd = FileLoaderServiceDescribe(nullptr, nullptr);
    ASSERT_NE(bd, nullptr);
    ASSERT_NE(bd->data, nullptr);
    EXPECT_TRUE(sJsonWellFormed(bd->data, bd->size)) << "raw: " << bd->data;
    EXPECT_TRUE(sContains(std::string(bd->data, bd->size), "\"error\":\"null handle\""));
    EXPECT_FALSE(sContains(std::string(bd->data, bd->size), "\"entries\""));
    BufferDescriptorRelease(bd);
    free(bd);
}

TEST(FileLoaderServiceDescribeTest, FreshServiceReportsConfigAndNoEntries)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 16;
    flc.service_config_params.loaded_size   = 4;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string js = sDescribe(flc.service_handle);
    ASSERT_FALSE(js.empty());
    EXPECT_TRUE(sJsonWellFormed(js.data(), js.size())) << "raw: " << js;
    EXPECT_TRUE(sContains(js, "\"size\":0"));
    EXPECT_TRUE(sContains(js, "\"capacity\":16"));
    EXPECT_TRUE(sContains(js, "\"buffer_cache_capacity\":4"));
    EXPECT_TRUE(sContains(js, "\"logger\":\"none\""));
    EXPECT_TRUE(sContains(js, "\"running\":true"));
    EXPECT_TRUE(sContains(js, "\"entries\":[]"));

    sStopAndDestroy(flc.service_handle);
}

TEST(FileLoaderServiceDescribeTest, ReportsRegisteredEntries)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string a = sTmpPath("describe_a");
    std::string b = sTmpPath("describe_b");
    sCreateFile(a.c_str(), "aaa");
    sCreateFile(b.c_str(), "bbbbb");
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, a.c_str()), (FileLoaderServiceFileHandle)0);
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, b.c_str()), (FileLoaderServiceFileHandle)0);

    std::string js = sDescribe(flc.service_handle);
    EXPECT_TRUE(sJsonWellFormed(js.data(), js.size())) << "raw: " << js;
    EXPECT_TRUE(sContains(js, "\"size\":2"));
    EXPECT_TRUE(sContains(js, a.c_str()));
    EXPECT_TRUE(sContains(js, b.c_str()));
    EXPECT_TRUE(sContains(js, "\"state\":\"active\""));
    EXPECT_EQ(sCount(js, "\"index\":"), 2u);
    EXPECT_EQ(std::count(js.begin(), js.end(), '['), 1);

    sStopAndDestroy(flc.service_handle);
    unlink(a.c_str());
    unlink(b.c_str());
}

TEST(FileLoaderServiceDescribeTest, AppendsToProvidedDescriptor)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 64);
    BufferDescriptorAppendFormatted(&bd, "MARKER\n");

    BufferDescriptor *ret = FileLoaderServiceDescribe(flc.service_handle, &bd);
    ASSERT_EQ(ret, &bd);
    std::string js(bd.data, bd.size);
    ASSERT_EQ(js.rfind("MARKER\n", 0), 0u) << "provided content was discarded";
    std::string tail = js.substr(7);
    EXPECT_TRUE(sJsonWellFormed(tail.data(), tail.size())) << "raw: " << tail;

    BufferDescriptorRelease(&bd);
    sStopAndDestroy(flc.service_handle);
}

TEST(FileLoaderServiceDescribeTest, EscapesQuotesAndBackslashesInPath)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 4;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string path = "/tmp/fls_test_quote\"back\\slash";
    sCreateFile(path.c_str(), "x");
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, path.c_str()), (FileLoaderServiceFileHandle)0);

    std::string js = sDescribe(flc.service_handle);
    EXPECT_TRUE(sJsonWellFormed(js.data(), js.size())) << "raw: " << js;
    EXPECT_TRUE(sContains(js, "quote\\\"back")) << "raw: " << js;
    EXPECT_TRUE(sContains(js, "back\\\\slash")) << "raw: " << js;

    sStopAndDestroy(flc.service_handle);
    unlink(path.c_str());
}

TEST(FileLoaderServiceDescribeTest, EscapesControlCharactersInPath)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 4;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string path = "/tmp/fls_test_ctl\x01\x1fname";
    sCreateFile(path.c_str(), "x");
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, path.c_str()), (FileLoaderServiceFileHandle)0);

    std::string js = sDescribe(flc.service_handle);
    EXPECT_TRUE(sJsonWellFormed(js.data(), js.size())) << "raw: " << js;
    EXPECT_TRUE(sContains(js, "\\u0001")) << "raw: " << js;
    EXPECT_TRUE(sContains(js, "\\u001f")) << "raw: " << js;

    sStopAndDestroy(flc.service_handle);
    unlink(path.c_str());
}

TEST(FileLoaderServiceDescribeTest, ReportsRunningFalseAfterStop)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    FileLoaderServiceStop(flc.service_handle);
    std::string js = sDescribe(flc.service_handle);
    EXPECT_TRUE(sJsonWellFormed(js.data(), js.size())) << "raw: " << js;
    EXPECT_TRUE(sContains(js, "\"running\":false"));

    FileLoaderServiceDestroy(flc.service_handle);
}

/* ==========================================================================
 * Logger injection
 * ========================================================================== */

TEST(FileLoaderServiceLoggerTest, NullLoggerBehavesLikePlainInit)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInitWithLogger(&flc, nullptr));
    ASSERT_NE(flc.service_handle, nullptr);

    std::string js = sDescribe(flc.service_handle);
    EXPECT_TRUE(sContains(js, "\"logger\":\"none\""));

    sStopAndDestroy(flc.service_handle);
}

TEST(FileLoaderServiceLoggerTest, SuppliedLoggerIsReportedEnabled)
{
    UfLogger *logger = sMakeLogger();
    if (!logger) GTEST_SKIP() << "no logger backend in this build";

    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInitWithLogger(&flc, logger));

    std::string js = sDescribe(flc.service_handle);
    EXPECT_TRUE(sJsonWellFormed(js.data(), js.size())) << "raw: " << js;
    EXPECT_TRUE(sContains(js, "\"logger\":\"enabled\""));

    sStopAndDestroy(flc.service_handle);
    UfLoggerDestroy(logger);
}

TEST(FileLoaderServiceLoggerTest, LoggerIsPerHandleNotProcessWide)
{
    UfLogger *logger = sMakeLogger();
    if (!logger) GTEST_SKIP() << "no logger backend in this build";

    FileLoaderServiceConcurrent logged = {};
    ASSERT_TRUE(FileLoaderServiceInitWithLogger(&logged, logger));

    FileLoaderServiceConcurrent plain = {};
    ASSERT_TRUE(FileLoaderServiceInit(&plain));

    EXPECT_TRUE(sContains(sDescribe(logged.service_handle), "\"logger\":\"enabled\""));
    EXPECT_TRUE(sContains(sDescribe(plain.service_handle), "\"logger\":\"none\""));

    sStopAndDestroy(logged.service_handle);
    sStopAndDestroy(plain.service_handle);
    UfLoggerDestroy(logger);
}

TEST(FileLoaderServiceLoggerTest, ServiceDoesNotConsumeSuppliedLogger)
{
    UfLogger *logger = sMakeLogger();
    if (!logger) GTEST_SKIP() << "no logger backend in this build";

    FileLoaderServiceConcurrent first = {};
    ASSERT_TRUE(FileLoaderServiceInitWithLogger(&first, logger));
    sStopAndDestroy(first.service_handle);

    FileLoaderServiceConcurrent second = {};
    ASSERT_TRUE(FileLoaderServiceInitWithLogger(&second, logger));
    EXPECT_TRUE(sContains(sDescribe(second.service_handle), "\"logger\":\"enabled\""));
    sStopAndDestroy(second.service_handle);

    UfLoggerDestroy(logger);
}

TEST(FileLoaderServiceLoggerTest, LoggerSuppliedServiceLoadsFiles)
{
    UfLogger *logger = sMakeLogger();
    if (!logger) GTEST_SKIP() << "no logger backend in this build";

    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    flc.service_config_params.loaded_size   = 4;
    ASSERT_TRUE(FileLoaderServiceInitWithLogger(&flc, logger));

    std::string path = sTmpPath("logger_load");
    sCreateFile(path.c_str(), "payload");
    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h, (FileLoaderServiceFileHandle)0);

    FileLoaderServiceRegisterThread(flc.service_handle);
    size_t size = 0;
    void  *data = nullptr;
    for (int attempt = 0; attempt < 50 && data == nullptr; attempt++) {
        data = FileLoaderServiceGetFileContent(flc.service_handle, h, &size);
        if (data == nullptr) usleep(20000);
    }
    EXPECT_NE(data, nullptr) << "file never became readable with a logger attached";
    if (data) FileLoaderServiceReturnFile(flc.service_handle, data);
    FileLoaderServiceUnregisterThread(flc.service_handle);

    sStopAndDestroy(flc.service_handle);
    UfLoggerDestroy(logger);
    unlink(path.c_str());
}

/* ==========================================================================
 * size reporting, retire and empty-file semantics
 * ========================================================================== */

static std::string sReadAll(const char *path) {
    std::string out;
    FILE *f = fopen(path, "r");
    if (!f) return out;
    char   buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static double sElapsedMs(const struct timespec &from) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - from.tv_sec) * 1000.0 + (now.tv_nsec - from.tv_nsec) / 1e6;
}

TEST(FileLoaderServiceTest, RemoveFileReleasesMapping)
{
    const char *needle = "/tmp/fls_test_relmap_";
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    flc.service_config_params.loaded_size   = 4;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::vector<std::string> paths;
    for (int i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof name, "relmap_%d", i);
        paths.push_back(sTmpPath(name));
        sCreateFile(paths.back().c_str(), "release me");
        ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, paths.back().c_str()),
                  (FileLoaderServiceFileHandle)0);
    }
    usleep(1500000);
    ASSERT_GE(sMappedFileCount(needle), 1) << "nothing was mapped, so the test would prove nothing";

    for (auto &p : paths) FileLoaderServiceRemoveFile(flc.service_handle, p.c_str());
    usleep(1500000);

    EXPECT_EQ(sMappedFileCount(needle), 0) << "mapping outlived the file's removal";
    sStopAndDestroy(flc.service_handle);
    for (auto &p : paths) unlink(p.c_str());
}

TEST(FileLoaderServiceTest, DestroyReleasesRemainingMappings)
{
    const char *needle = "/tmp/fls_test_destmap_";
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    flc.service_config_params.loaded_size   = 4;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::vector<std::string> paths;
    for (int i = 0; i < 2; i++) {
        char name[64];
        snprintf(name, sizeof name, "destmap_%d", i);
        paths.push_back(sTmpPath(name));
        sCreateFile(paths.back().c_str(), "destroy me");
        ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, paths.back().c_str()),
                  (FileLoaderServiceFileHandle)0);
    }
    usleep(1500000);
    ASSERT_GE(sMappedFileCount(needle), 1) << "nothing was mapped, so the test would prove nothing";

    sStopAndDestroy(flc.service_handle);

    EXPECT_EQ(sMappedFileCount(needle), 0) << "destroy left mappings behind";
    for (auto &p : paths) unlink(p.c_str());
}

TEST(FileLoaderServiceTest, RemoveDuringLoadDoesNotLeakMappings)
{
    const char *needle = "/tmp/fls_test_churnmap_";
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 64;
    flc.service_config_params.loaded_size   = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    std::vector<std::string> paths;
    for (int i = 0; i < 8; i++) {
        char name[64];
        snprintf(name, sizeof name, "churnmap_%d", i);
        paths.push_back(sTmpPath(name));
        sCreateFile(paths.back().c_str(), "churn payload");
    }
    FileLoaderServiceFileHandle h0 = FileLoaderServiceAddFile(flc.service_handle, paths[0].c_str());
    ASSERT_NE(h0, (FileLoaderServiceFileHandle)0);
    size_t sz = 0;
    void  *d  = nullptr;
    for (int i = 0; i < 200 && !d; i++) {
        d = FileLoaderServiceGetFileContent(flc.service_handle, h0, &sz);
        if (!d) usleep(10000);
    }
    ASSERT_NE(d, nullptr) << "the service never loaded, so the test would prove nothing";
    FileLoaderServiceReturnFile(flc.service_handle, d);
    ASSERT_GE(sMappedFileCount(needle), 1);
    FileLoaderServiceRemoveFile(flc.service_handle, paths[0].c_str());

    for (int i = 0; i < 20000; i++) {
        const char *p = paths[(size_t)i % paths.size()].c_str();
        FileLoaderServiceAddFile(flc.service_handle, p);
        FileLoaderServiceRemoveFile(flc.service_handle, p);
    }

    FileLoaderServiceUnregisterThread(flc.service_handle);
    sStopAndDestroy(flc.service_handle);

    EXPECT_EQ(sMappedFileCount(needle), 0) << "a mapping was dropped when a removal overtook a load";
    for (auto &p : paths) unlink(p.c_str());
}

TEST(FileLoaderServiceTest, GetFileContentReturnsLengthAndBytes)
{
    static const char payload[] = "round-trip payload 0123456789";
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    std::string path = sTmpPath("sizeof_content");
    sCreateFile(path.c_str(), payload);
    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h, (FileLoaderServiceFileHandle)0);

    size_t size  = 0;
    void  *data  = nullptr;
    for (int i = 0; i < 200 && data == nullptr; i++) {
        data = FileLoaderServiceGetFileContent(flc.service_handle, h, &size);
        if (data == nullptr) usleep(20000);
    }
    ASSERT_NE(data, nullptr) << "content never became available to the caller";
    EXPECT_EQ(size, sizeof payload - 1) << "returned length is not the file's length";
    EXPECT_EQ(memcmp(data, payload, sizeof payload - 1), 0) << "returned bytes are not the file's bytes";
    FileLoaderServiceReturnFile(flc.service_handle, data);

    FileLoaderServiceUnregisterThread(flc.service_handle);
    sStopAndDestroy(flc.service_handle);
    unlink(path.c_str());
}

TEST(FileLoaderServiceTest, GetFileContentByPathServesRegisteredFile)
{
    static const char payload[] = "path-based payload 0123456789";
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    std::string path = sTmpPath("by_path");
    sCreateFile(path.c_str(), payload);
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, path.c_str()), (FileLoaderServiceFileHandle)0);

    size_t size = 0;
    void  *data = nullptr;
    for (int i = 0; i < 200 && data == nullptr; i++) {
        data = FileLoaderServiceGetFileContentByPath(flc.service_handle, path.c_str(), &size);
        if (data == nullptr) usleep(20000);
    }
    ASSERT_NE(data, nullptr) << "content never became available by path";
    EXPECT_EQ(size, sizeof payload - 1) << "returned length is not the file's length";
    EXPECT_EQ(memcmp(data, payload, sizeof payload - 1), 0) << "returned bytes are not the file's bytes";
    FileLoaderServiceReturnFile(flc.service_handle, data);

    size = 1;
    errno = 0;
    EXPECT_EQ(FileLoaderServiceGetFileContentByPath(flc.service_handle, "/tmp/fls_not_registered", &size), nullptr);
    EXPECT_EQ(errno, ENOENT) << "unregistered path must report ENOENT";
    EXPECT_EQ(size, 0u) << "outSize must be zeroed on failure";

    errno = 0;
    EXPECT_EQ(FileLoaderServiceGetFileContentByPath(flc.service_handle, "", &size), nullptr);
    EXPECT_EQ(errno, EINVAL) << "empty path must report EINVAL";

    FileLoaderServiceUnregisterThread(flc.service_handle);
    sStopAndDestroy(flc.service_handle);
    unlink(path.c_str());
}

TEST(FileLoaderServiceTest, GetFileContentByPathRejectsBlacklisted)
{
    const char *bl[] = { "/tmp/fls_by_path_blacklisted", nullptr };
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size       = 8;
    flc.service_config_params.blacklist_entries   = bl;
    flc.service_config_params.blacklist_entry_count = 1;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    sCreateFile("/tmp/fls_by_path_blacklisted", "secret");

    size_t size = 1;
    errno = 0;
    EXPECT_EQ(FileLoaderServiceGetFileContentByPath(flc.service_handle, "/tmp/fls_by_path_blacklisted", &size), nullptr);
    EXPECT_EQ(errno, EACCES) << "blacklisted path must report EACCES";
    EXPECT_EQ(size, 0u) << "outSize must be zeroed on failure";

    FileLoaderServiceUnregisterThread(flc.service_handle);
    sStopAndDestroy(flc.service_handle);
    unlink("/tmp/fls_by_path_blacklisted");
}

TEST(FileLoaderServiceTest, GetFileContentByPathRejectsInvalidArguments)
{
    size_t size = 1;
    errno = 0;
    EXPECT_EQ(FileLoaderServiceGetFileContentByPath(nullptr, "/tmp/x", &size), nullptr);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(size, 0u);
}

TEST(FileLoaderServiceTest, GetFileInfoReportsSizeBytes)
{
    static const char payload[] = "info size payload";
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string path = sTmpPath("sizeof_info");
    sCreateFile(path.c_str(), payload);
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, path.c_str()), (FileLoaderServiceFileHandle)0);

    FileLoaderServiceFileInfo info;
    for (int i = 0; i < 100; i++) {
        ASSERT_TRUE(FileLoaderServiceGetFileInfo(flc.service_handle, path.c_str(), &info));
        if (info.is_loaded) break;
        usleep(20000);
    }
    EXPECT_TRUE(info.is_loaded);
    EXPECT_EQ(info.size_bytes, sizeof payload - 1);
    EXPECT_EQ(info.path[0], '/');

    sStopAndDestroy(flc.service_handle);
    unlink(path.c_str());
}

TEST(FileLoaderServiceTest, DescribeReportsSizeBytes)
{
    static const char payload[] = "describe size payload";
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string path = sTmpPath("sizeof_describe");
    sCreateFile(path.c_str(), payload);
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, path.c_str()), (FileLoaderServiceFileHandle)0);

    std::string js;
    for (int i = 0; i < 100; i++) {
        js = sDescribe(flc.service_handle);
        if (sContains(js, "\"loaded\":true")) break;
        usleep(20000);
    }
    EXPECT_TRUE(sJsonWellFormed(js.data(), js.size())) << js;
    EXPECT_TRUE(sContains(js, ("\"size_bytes\":" + std::to_string(sizeof payload - 1)).c_str())) << js;

    sStopAndDestroy(flc.service_handle);
    unlink(path.c_str());
}

TEST(FileLoaderServiceTest, GetFileContentRejectsInvalidHandleAndZeroesSize)
{
    FileLoaderServiceConcurrent flc = {};
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    size_t size = 12345;
    EXPECT_EQ(FileLoaderServiceGetFileContent(nullptr, 1, &size), nullptr);
    EXPECT_EQ(size, 0u) << "outSize left stale on the null-handle path";

    size = 12345;
    EXPECT_EQ(FileLoaderServiceGetFileContent(flc.service_handle, 0, &size), nullptr);
    EXPECT_EQ(size, 0u) << "outSize left stale on the invalid-handle path";

    size = 12345;
    FileLoaderServiceFileHandle bogus = ((FileLoaderServiceFileHandle)3 << 44) | 1;
    EXPECT_EQ(FileLoaderServiceGetFileContent(flc.service_handle, bogus, &size), nullptr);
    EXPECT_EQ(size, 0u) << "outSize left stale on the stale-handle path";

    sStopAndDestroy(flc.service_handle);
}

TEST(FileLoaderServiceTest, EmptyFileReadsAsZeroLength)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));
    FileLoaderServiceRegisterThread(flc.service_handle);

    std::string path = sTmpPath("empty_read");
    sCreateFile(path.c_str(), "");
    FileLoaderServiceFileHandle h = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h, (FileLoaderServiceFileHandle)0);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    size_t size = 999;
    void  *data = FileLoaderServiceGetFileContent(flc.service_handle, h, &size);
    double ms   = sElapsedMs(start);

    ASSERT_NE(data, nullptr) << "empty file never became readable";
    EXPECT_EQ(size, 0u);
    EXPECT_LT(ms, 2000.0) << "empty file blocked the caller until the service timeout";
    FileLoaderServiceReturnFile(flc.service_handle, data);

    FileLoaderServiceUnregisterThread(flc.service_handle);
    sStopAndDestroy(flc.service_handle);
    unlink(path.c_str());
}

TEST(FileLoaderServiceTest, DebugBuildTracesInitAndLoad)
{
    UfLogger *logger = sMakeLogger();
    if (!logger) GTEST_SKIP() << "no logger backend in this build";

    std::string log_path = sTmpPath("debug_trace.log");
    unlink(log_path.c_str());
    if (true) {
        UfLoggerConfig cfg = *UfLoggerProvideSaneDefaults();
        cfg.destination   = UF_LOGGER_DESTINATION_FILE;
        cfg.file_path     = log_path.c_str();
        cfg.minimum_level = UF_LOGGER_LEVEL_DEBUG;
        UfLogger *file_logger = nullptr;
        ASSERT_EQ(UfLoggerCreate(&cfg, &file_logger), UF_LOGGER_STATUS_OK);
        UfLoggerDestroy(logger);
        logger = file_logger;
    }

    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    flc.service_config_params.loaded_size   = 2;
    ASSERT_TRUE(FileLoaderServiceInitWithLogger(&flc, logger));

    std::string path = sTmpPath("trace_load");
    sCreateFile(path.c_str(), "trace payload");
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, path.c_str()), (FileLoaderServiceFileHandle)0);
    usleep(1500000);

    sStopAndDestroy(flc.service_handle);
    UfLoggerDestroy(logger);

    std::string log = sReadAll(log_path.c_str());
    EXPECT_TRUE(sContains(log, "init: resolved pool_capacity=8")) << log;
    EXPECT_TRUE(sContains(log, "init: describe ")) << log;
    EXPECT_TRUE(sContains(log, "load start: ")) << log;
    EXPECT_TRUE(sContains(log, "loaded: ")) << log;
    EXPECT_TRUE(sContains(log, "trace payload") == false) << "log leaked file content";

    unlink(path.c_str());
    unlink(log_path.c_str());
}

static size_t sTopLevelCounter(const std::string &js, const char *key) {
    std::string needle = std::string("\"") + key + "\":";
    size_t      at     = js.find(needle);
    if (at == std::string::npos) return (size_t)-1;
    return (size_t)strtoul(js.c_str() + at + needle.size(), nullptr, 10);
}

TEST(FileLoaderServiceTest, CacheAccountingTracksOccupiedSlots)
{
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    flc.service_config_params.loaded_size   = 2;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string paths[3];
    for (int i = 0; i < 2; i++) {
        char name[64];
        snprintf(name, sizeof name, "accmap_%d", i);
        paths[i] = sTmpPath(name);
        sCreateFile(paths[i].c_str(), "accounting payload");
        ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, paths[i].c_str()),
                  (FileLoaderServiceFileHandle)0);
    }
    usleep(1500000);

    FileLoaderServiceRemoveFile(flc.service_handle, paths[0].c_str());
    paths[2] = sTmpPath("accmap_2");
    sCreateFile(paths[2].c_str(), "accounting payload");
    ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, paths[2].c_str()),
              (FileLoaderServiceFileHandle)0);
    usleep(2000000);

    std::string js      = sDescribe(flc.service_handle);
    size_t      slots   = sCount(js, "\"in_cache\":true");
    size_t      counter = sTopLevelCounter(js, "loaded");

    EXPECT_LE(slots, 2u) << "cache holds more entries than its capacity\n" << js;
    EXPECT_EQ(counter, slots)
        << "cache_loaded_count disagrees with the number of occupied slots\n" << js;

    sStopAndDestroy(flc.service_handle);
    for (auto &p : paths) unlink(p.c_str());
}
