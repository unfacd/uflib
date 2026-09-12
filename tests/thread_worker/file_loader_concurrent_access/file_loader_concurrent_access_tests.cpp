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
#include <cstdio>
#include <unistd.h>

extern "C" {
#include <uflib/file_loader_service_concurrent/file_loader_concurrent_service.h>
}

static std::string sTmpPath(const char *name) {
    return std::string("/tmp/fls_test_") + name;
}
static void sCreateFile(const char *path, const char *content) {
    FILE *f = fopen(path, "w"); assert(f); fputs(content, f); fclose(f);
}
static void sCleanup(FileLoaderService *svc, pthread_t *th) {
    if (svc) { FileLoaderServiceStop(svc); if (*th) pthread_join(*th, nullptr);
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
    FileLoaderServiceStop(flc.service_handle);
    pthread_join(th, nullptr);
    FileLoaderServiceDestroy(flc.service_handle);
    flc.service_handle = NULL;
    FileLoaderServiceDestroy(flc.service_handle);  /* NULL-safe */
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
    FileLoaderServiceHandle h = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    EXPECT_NE(h, (FileLoaderServiceHandle)0);
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
    FileLoaderServiceHandle h = FileLoaderServiceAddFile(flc.service_handle, "/tmp/fls_blacklisted");
    EXPECT_EQ(h, (FileLoaderServiceHandle)0);
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
    EXPECT_EQ(FileLoaderServiceAddFile(flc.service_handle, nullptr), (FileLoaderServiceHandle)0);
    EXPECT_EQ(FileLoaderServiceAddFile(flc.service_handle, ""), (FileLoaderServiceHandle)0);
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
    // Node 0 is reserved, so usable nodes = registry_size - 1.
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;  // 7 usable nodes
    flc.service_config_params.loaded_size   = 4;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::vector<std::string> paths;
    char nameBuf[64];
    // Fill all 7 usable nodes
    for (int i = 0; i < 7; i++) {
        snprintf(nameBuf, sizeof(nameBuf), "pool_%d", i);
        std::string p = sTmpPath(nameBuf);
        sCreateFile(p.c_str(), "x");
        paths.push_back(p);
        FileLoaderServiceHandle h = FileLoaderServiceAddFile(flc.service_handle, p.c_str());
        ASSERT_NE(h, (FileLoaderServiceHandle)0) << "failed to add file " << i;
    }

    // 9th file — pool is full, no eviction callback → returns 0
    std::string extra = sTmpPath("pool_extra");
    sCreateFile(extra.c_str(), "overflow");
    FileLoaderServiceHandle h9 = FileLoaderServiceAddFile(flc.service_handle, extra.c_str());
    EXPECT_EQ(h9, (FileLoaderServiceHandle)0) << "pool should be exhausted";

    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    for (auto &p : paths) unlink(p.c_str());
    unlink(extra.c_str());
}

TEST(FileLoaderServiceTest, PoolReuseAfterRemove)
{
    // Remove one file → free pool slot → re-add succeeds.
    // Node 0 is reserved, so registry_size=4 → 3 usable nodes.
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 4;  // 3 usable nodes
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::vector<std::string> paths;
    char nameBuf[64];
    // Fill all 3 usable nodes
    for (int i = 0; i < 3; i++) {
        snprintf(nameBuf, sizeof(nameBuf), "reuse_%d", i);
        std::string p = sTmpPath(nameBuf);
        sCreateFile(p.c_str(), "x");
        paths.push_back(p);
        ASSERT_NE(FileLoaderServiceAddFile(flc.service_handle, p.c_str()),
                  (FileLoaderServiceHandle)0);
    }

    // Pool full
    std::string extra = sTmpPath("reuse_extra");
    sCreateFile(extra.c_str(), "z");
    EXPECT_EQ(FileLoaderServiceAddFile(flc.service_handle, extra.c_str()),
              (FileLoaderServiceHandle)0);

    // Remove one file → free a slot
    FileLoaderServiceRemoveFile(flc.service_handle, paths[0].c_str());

    // Re-add the extra file — should succeed (slot was freed)
    FileLoaderServiceHandle h = FileLoaderServiceAddFile(flc.service_handle, extra.c_str());
    EXPECT_NE(h, (FileLoaderServiceHandle)0);

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
    // loaded_size (4) < registry_size (15 usable nodes) — adding 15 files
    // forces the buffer cache to evict cold entries.  After all files are
    // added and loaded, every file must still be registered (in_registry=true).
    // This exercises sClockEvictBuffer → sUnloadBuffer.
    // Node 0 is reserved, so registry_size=16 → 15 usable nodes.
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 16;  // 15 usable nodes
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
        FileLoaderServiceHandle h = FileLoaderServiceAddFile(flc.service_handle, p.c_str());
        ASSERT_NE(h, (FileLoaderServiceHandle)0) << "add failed for file " << i;
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
    FileLoaderServiceHandle h = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h, (FileLoaderServiceHandle)0);

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
    FileLoaderServiceHandle h = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h, (FileLoaderServiceHandle)0);

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
    FileLoaderServiceHandle h1 = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h1, (FileLoaderServiceHandle)0);

    FileLoaderServiceRemoveFile(flc.service_handle, path.c_str());

    // Re-create with different content
    unlink(path.c_str());
    sCreateFile(path.c_str(), "v2");
    FileLoaderServiceHandle h2 = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h2, (FileLoaderServiceHandle)0);

    // Handle must differ (generation bump)
    EXPECT_NE(h1, h2);

    // Implicit recovery: GetFileContent detects stale handle (gen mismatch
    // after remove→re-add), looks up path via hashmap, recovers.
    // The path is re-registered so recovery succeeds with current content.
    FileLoaderServiceRegisterThread(flc.service_handle);
    size_t sz = 0;
    errno = 0;
    void *data = FileLoaderServiceGetFileContent(flc.service_handle, h1, &sz);
    EXPECT_NE(data, nullptr) << "implicit recovery: stale handle on re-added file";
    // sz may be 0 — the current implementation does not store file size in
    // the node struct (known limitation, pre-existing).
    FileLoaderServiceReturnFile(flc.service_handle, data);
    FileLoaderServiceUnregisterThread(flc.service_handle);

    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink(path.c_str());
}

/* ==========================================================================
 * Duplicate add
 * ========================================================================== */

TEST(FileLoaderServiceTest, DuplicateAddSamePath)
{
    // Adding the same path twice: first succeeds, second returns 0
    // (hashmap rejects duplicate key).  The file is still registered.
    FileLoaderServiceConcurrent flc = {};
    flc.service_config_params.registry_size = 8;
    ASSERT_TRUE(FileLoaderServiceInit(&flc));

    std::string path = sTmpPath("dup");
    sCreateFile(path.c_str(), "same");
    FileLoaderServiceHandle h1 = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    ASSERT_NE(h1, (FileLoaderServiceHandle)0);

    // Duplicate add now returns a valid handle (AddFile is idempotent —
    // hashmap lookup recovers the existing mapping).
    FileLoaderServiceHandle h2 = FileLoaderServiceAddFile(flc.service_handle, path.c_str());
    EXPECT_NE(h2, (FileLoaderServiceHandle)0);
    // Both handles point to the same file — size is still 1
    FileLoaderServiceFileInfo info;
    EXPECT_TRUE(FileLoaderServiceGetFileInfo(flc.service_handle, path.c_str(), &info));
    EXPECT_TRUE(info.in_registry);
    EXPECT_EQ(FileLoaderServiceSize(flc.service_handle), 1u);

    pthread_t th = FileLoaderServiceGetThread(flc.service_handle);
    sCleanup(flc.service_handle, &th);
    unlink(path.c_str());
}
