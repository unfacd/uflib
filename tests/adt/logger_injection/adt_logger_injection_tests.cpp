/**
 * @file adt_logger_injection_tests.cpp
 * @brief The adt modules' additive logger seam, held in place.
 *
 * Three adt modules match the agreed shape — an opaque handle created by a
 * function that returns it: `adt_minheap` (two constructors, pointer-key and
 * i64-key), `hashtable_v2` and `hopscotch_hashtable_v2` (both config-struct
 * constructors).  The remaining adt modules take a caller-allocated struct and
 * therefore do not match the shape; they are not covered here.
 *
 * The suite is written to fail when the contract is broken rather than to
 * record what the code currently does:
 *
 *   - **NULL is the old behaviour.**  Every legacy constructor forwards NULL, so
 *     a caller that never heard of the logger must see exactly what it saw
 *     before, and a legacy constructor must emit nothing at all.
 *
 *   - **The borrow reaches the sink, and is not ownership.**  After a module is
 *     destroyed the logger is still used: if the module had freed what it only
 *     borrowed, that is a use-after-free and the sanitizer says so.
 *
 *   - **Nothing is logged on an operational path.**  A long run of insert /
 *     lookup / remove with the floor at TRACE must emit zero records.  That is
 *     the assertion that fails if a log site is ever added to an operation.
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

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include <uflib/standard_c_includes.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/adt/adt_minheap.h>
#include <uflib/adt/hashtable_v2/hashtable_v2.h>
#include <uflib/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2.h>
}

#include "mock_driver.h"

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

TEST(AdtLoggerInjectionNullLogger, LegacyConstructorsEmitNothing)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    /* The legacy constructors take no logger, so they must not merely pass NULL
       internally — they must produce no record on a logger that exists and is
       listening at the lowest possible floor. */
    MinHeap *ptr_heap = MinHeapCreate(16, MinHeapCompareIntKeys);
    ASSERT_NE(ptr_heap, nullptr);
    MinHeapDestroy(ptr_heap);

    MinHeap *i64_heap = MinHeapCreateI64(16);
    ASSERT_NE(i64_heap, nullptr);
    MinHeapDestroy(i64_heap);

    HashTableV2 *ht_ptr = HashTableV2Create(nullptr);
    ASSERT_NE(ht_ptr, nullptr);
    HashTableV2Destroy(ht_ptr);

    HopscotchHashTable *hop_ptr = HopscotchHashTableCreate(nullptr);
    ASSERT_NE(hop_ptr, nullptr);
    HopscotchHashTableDestroy(hop_ptr);

    EXPECT_EQ(MockDriverWriteCount(), 0)
        << "a legacy constructor reached a logger it was never given";

    UfLoggerDestroy(log_ptr);
}

TEST(AdtLoggerInjectionNullLogger, MinHeapWithNullLoggerBehavesAsBefore)
{
    MinHeap *h = MinHeapCreateWithLogger(64, MinHeapCompareIntKeys, nullptr);
    ASSERT_NE(h, nullptr);

    int keys[4] = {40, 10, 30, 20};
    int values[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(MinHeapInsert(h, &keys[i], &values[i]), MINHEAP_OK);
    }
    EXPECT_EQ(MinHeapSize(h), 4);

    void *min_key = nullptr;
    void *min_value = nullptr;
    /* The query/remove entry points report 1 on success and 0 on empty — not
       MINHEAP_OK, which is the insert path's convention. */
    ASSERT_EQ(MinHeapMin(h, &min_key, &min_value), 1);
    EXPECT_EQ(*(int *)min_key, 10);

    MinHeapDestroy(h);
}

TEST(AdtLoggerInjectionNullLogger, MinHeapI64WithNullLoggerBehavesAsBefore)
{
    MinHeap *h = MinHeapCreateI64WithLogger(32, nullptr);
    ASSERT_NE(h, nullptr);

    int value = 7;
    ASSERT_EQ(MinHeapInsertI64(h, 20, &value), MINHEAP_OK);
    ASSERT_EQ(MinHeapInsertI64(h, 10, &value), MINHEAP_OK);

    int64_t key = 0;
    void *out_ptr = nullptr;
    ASSERT_EQ(MinHeapDelminI64(h, &key, &out_ptr), 1);
    EXPECT_EQ(key, 10);
    EXPECT_EQ(out_ptr, &value);

    MinHeapDestroy(h);
}

TEST(AdtLoggerInjectionNullLogger, HashTableV2WithNullLoggerBehavesAsBefore)
{
    HashTableV2Config cfg = {};
    cfg.name = "AdtInjectionNull";
    HashTableV2 *ht_ptr = HashTableV2CreateWithLogger(&cfg, nullptr);
    ASSERT_NE(ht_ptr, nullptr);

    char key[] = "alpha";
    bool added = false;
    ASSERT_NE(HashTableV2Insert(ht_ptr, key, &added), nullptr);
    /* Despite the parameter's name, it reports "the item was ALREADY present",
       so false on a fresh insert means the new item was stored.  The header
       documents this; the name reads the other way. */
    EXPECT_FALSE(added);
    EXPECT_EQ(HashTableV2Size(ht_ptr), 1u);
    EXPECT_EQ(HashTableV2Lookup(ht_ptr, key), (HashTableV2Item *)key);

    HashTableV2Destroy(ht_ptr);
}

TEST(AdtLoggerInjectionNullLogger, HopscotchWithNullLoggerBehavesAsBefore)
{
    HopscotchHashTableConfig cfg = {};
    cfg.pfactor = 6;
    cfg.key_len = sizeof(uint64_t);
    cfg.key_offset = 0;

    HopscotchHashTable *ht_ptr = HopscotchHashTableCreateWithLogger(&cfg, nullptr);
    ASSERT_NE(ht_ptr, nullptr);
    EXPECT_TRUE(HopscotchHashTableIsEmpty(ht_ptr));

    uint64_t key = 42u;
    ASSERT_EQ(HopscotchHashTableInsert(ht_ptr, &key), 0);
    EXPECT_EQ(HopscotchHashTableSize(ht_ptr), 1u);
    EXPECT_EQ(HopscotchHashTableLookup(ht_ptr, (const uint8_t *)&key), (void *)&key);

    HopscotchHashTableDestroy(ht_ptr);
}

/* ── The borrow reaches the sink ────────────────────────────────────────── */

TEST(AdtLoggerInjectionReporting, MinHeapReportsCreateAndRejection)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    MinHeap *h = MinHeapCreateWithLogger(64, MinHeapCompareIntKeys, log_ptr);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1)
        << "creation was not reported";

    /* A negative capacity used to reach the caller as a bare NULL. */
    MockDriverReset();
    EXPECT_EQ(MinHeapCreateWithLogger(-1, MinHeapCompareIntKeys, log_ptr), nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_ERROR), 1);

    MinHeapDestroy(h);
    UfLoggerDestroy(log_ptr);
}

TEST(AdtLoggerInjectionReporting, MinHeapI64ReportsCreate)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    MockDriverReset();
    MinHeap *h = MinHeapCreateI64WithLogger(32, log_ptr);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1)
        << "the i64 constructor is not wired to the logger";

    MockDriverReset();
    MinHeapDestroy(h);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    UfLoggerDestroy(log_ptr);
}

TEST(AdtLoggerInjectionReporting, HashTableV2ReportsCreateAndNamesTheTable)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    HashTableV2Config cfg = {};
    cfg.name = "SessionsByID";

    MockDriverReset();
    HashTableV2 *ht_ptr = HashTableV2CreateWithLogger(&cfg, log_ptr);
    ASSERT_NE(ht_ptr, nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    /* The record must carry the table's own name: a process running several
       tables is otherwise unable to tell which one reported. */
    const MockDriverRecord *record_ptr = MockDriverLastRecord();
    ASSERT_NE(record_ptr, nullptr);
    EXPECT_NE(std::strstr(record_ptr->message, "SessionsByID"), nullptr)
        << "record does not name the table; message was: " << record_ptr->message;

    MockDriverReset();
    HashTableV2Destroy(ht_ptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    UfLoggerDestroy(log_ptr);
}

TEST(AdtLoggerInjectionReporting, HopscotchReportsBothEnds)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    MockDriverReset();
    HopscotchHashTable *ht_ptr = HopscotchHashTableCreateWithLogger(nullptr, log_ptr);
    ASSERT_NE(ht_ptr, nullptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    MockDriverReset();
    HopscotchHashTableDestroy(ht_ptr);
    EXPECT_EQ(MockDriverWriteCountForLevel(UF_LOGGER_LEVEL_DEBUG), 1);

    UfLoggerDestroy(log_ptr);
}

/* ── Introspection reports the borrow, where there is introspection ─────── */

TEST(AdtLoggerInjectionDescribe, MinHeapReportsEnabledOnlyWhenALoggerWasInjected)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    /* Side by side: an answer hardcoded in either direction makes these two
       describe the same thing, which is what the pair is here to catch. */
    MinHeap *with_ptr = MinHeapCreateWithLogger(16, MinHeapCompareIntKeys, log_ptr);
    ASSERT_NE(with_ptr, nullptr);
    MinHeap *without_ptr = MinHeapCreateWithLogger(16, MinHeapCompareIntKeys, nullptr);
    ASSERT_NE(without_ptr, nullptr);

    BufferDescriptor with_bd;
    BufferDescriptorInit(&with_bd, 512);
    ASSERT_EQ(MinHeapDescribe(with_ptr, &with_bd), &with_bd);
    EXPECT_NE(std::strstr(with_bd.data, "\"logger\":\"enabled\""), nullptr)
        << "got: " << with_bd.data;
    BufferDescriptorRelease(&with_bd);

    BufferDescriptor without_bd;
    BufferDescriptorInit(&without_bd, 512);
    ASSERT_EQ(MinHeapDescribe(without_ptr, &without_bd), &without_bd);
    EXPECT_NE(std::strstr(without_bd.data, "\"logger\":\"none\""), nullptr)
        << "got: " << without_bd.data;
    BufferDescriptorRelease(&without_bd);

    MinHeapDestroy(with_ptr);
    MinHeapDestroy(without_ptr);
    UfLoggerDestroy(log_ptr);
}

TEST(AdtLoggerInjectionDescribe, MinHeapStaysEnabledAfterUse)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    /* The attribute reports a borrow, and a borrow does not lapse because the
       heap was used.  If it were derived from anything but the stored pointer,
       this would show it. */
    MinHeap *h = MinHeapCreateI64WithLogger(16, log_ptr);
    ASSERT_NE(h, nullptr);

    int value = 1;
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(MinHeapInsertI64(h, i, &value), MINHEAP_OK);
    }
    int64_t key = 0;
    void *out_ptr = nullptr;
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(MinHeapDelminI64(h, &key, &out_ptr), 1);
    }

    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 512);
    ASSERT_EQ(MinHeapDescribe(h, &bd), &bd);
    EXPECT_NE(std::strstr(bd.data, "\"logger\":\"enabled\""), nullptr)
        << "got: " << bd.data;
    BufferDescriptorRelease(&bd);

    MinHeapDestroy(h);
    UfLoggerDestroy(log_ptr);
}

TEST(AdtLoggerInjectionDescribe, NullHandleCarriesNoLoggerAttribute)
{
    /* A handle that does not exist has no borrow, so claiming one — in either
       direction — would be a fabricated answer. */
    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 256);
    ASSERT_EQ(MinHeapDescribe(nullptr, &bd), &bd);
    EXPECT_NE(std::strstr(bd.data, "\"error\":\"null handle\""), nullptr);
    EXPECT_EQ(std::strstr(bd.data, "logger"), nullptr)
        << "got: " << bd.data;
    BufferDescriptorRelease(&bd);
}

/* ── The borrow is not ownership ────────────────────────────────────────── */

TEST(AdtLoggerInjectionBorrow, ModulesDoNotDestroyWhatTheyBorrowed)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    HashTableV2Config ht_cfg = {};
    ht_cfg.name = "BorrowCheck";

    MinHeap *heap_ptr = MinHeapCreateWithLogger(8, MinHeapCompareIntKeys, log_ptr);
    ASSERT_NE(heap_ptr, nullptr);
    MinHeap *i64_ptr = MinHeapCreateI64WithLogger(8, log_ptr);
    ASSERT_NE(i64_ptr, nullptr);
    HashTableV2 *ht_ptr = HashTableV2CreateWithLogger(&ht_cfg, log_ptr);
    ASSERT_NE(ht_ptr, nullptr);
    HopscotchHashTable *hop_ptr = HopscotchHashTableCreateWithLogger(nullptr, log_ptr);
    ASSERT_NE(hop_ptr, nullptr);

    /* Every borrower released, the logger deliberately not. */
    MinHeapDestroy(heap_ptr);
    MinHeapDestroy(i64_ptr);
    HashTableV2Destroy(ht_ptr);
    HopscotchHashTableDestroy(hop_ptr);

    /* Using the logger now is the point: a module that freed what it only
       borrowed makes this a use-after-free, which LSan reports rather than the
       expectation merely failing. */
    MockDriverReset();
    ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "borrow outlived every adt module"),
              UF_LOGGER_STATUS_OK);
    EXPECT_EQ(MockDriverWriteCount(), 1);

    UfLoggerDestroy(log_ptr);
}

/* ── Nothing on an operational path ─────────────────────────────────────── */

TEST(AdtLoggerInjectionOperations, MinHeapOperationsEmitNothing)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    MinHeap *h = MinHeapCreateI64WithLogger(64, log_ptr);
    ASSERT_NE(h, nullptr);

    /* Everything from here is an operational path.  The floor is TRACE and the
       double is listening: if a log site existed on insert or delmin, it would
       fire here. */
    MockDriverReset();

    constexpr int kOps = 5000;
    int value = 1;
    for (int i = 0; i < kOps; ++i) {
        ASSERT_EQ(MinHeapInsertI64(h, i, &value), MINHEAP_OK);
    }
    int64_t key = 0;
    void *out_ptr = nullptr;
    for (int i = 0; i < kOps; ++i) {
        ASSERT_EQ(MinHeapDelminI64(h, &key, &out_ptr), 1);
    }
    EXPECT_EQ(MinHeapSize(h), 0);

    EXPECT_EQ(MockDriverWriteCount(), 0)
        << "a record was emitted from an operational path; logging must stay off it";

    MinHeapDestroy(h);
    UfLoggerDestroy(log_ptr);
}

TEST(AdtLoggerInjectionOperations, HashTableV2OperationsEmitNothing)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    HashTableV2Config cfg = {};
    cfg.name = "OperationCheck";
    HashTableV2 *ht_ptr = HashTableV2CreateWithLogger(&cfg, log_ptr);
    ASSERT_NE(ht_ptr, nullptr);

    MockDriverReset();

    /* Each key is a live c-string for the whole run, because the table stores
       the caller's pointer and does not copy. */
    static char keys[64][8];
    for (int i = 0; i < 64; ++i) {
        std::snprintf(keys[i], sizeof(keys[i]), "k%d", i);
        bool added = false;
        ASSERT_NE(HashTableV2Insert(ht_ptr, keys[i], &added), nullptr);
    }
    ASSERT_EQ(HashTableV2Size(ht_ptr), 64u) << "the operations did not actually do anything";

    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 64; ++i) {
            (void)HashTableV2Lookup(ht_ptr, keys[i]);
        }
        (void)HashTableV2Capacity(ht_ptr);
    }
    for (int i = 0; i < 64; ++i) {
        (void)HashTableV2Remove(ht_ptr, keys[i]);
    }

    EXPECT_EQ(MockDriverWriteCount(), 0)
        << "a record was emitted from an operational path; logging must stay off it";

    HashTableV2Destroy(ht_ptr);
    UfLoggerDestroy(log_ptr);
}

TEST(AdtLoggerInjectionOperations, HopscotchOperationsEmitNothing)
{
    MockDriverReset();
    UfLogger *log_ptr = MakeRecordingLogger();
    ASSERT_NE(log_ptr, nullptr);

    HopscotchHashTableConfig cfg = {};
    cfg.pfactor = 8;
    cfg.key_len = sizeof(uint64_t);
    cfg.key_offset = 0;
    HopscotchHashTable *ht_ptr = HopscotchHashTableCreateWithLogger(&cfg, log_ptr);
    ASSERT_NE(ht_ptr, nullptr);

    MockDriverReset();

    /* Keys outlive the table: hopscotch stores the caller's pointer. */
    std::vector<uint64_t> keys(256u);
    for (uint64_t i = 0; i < keys.size(); ++i) {
        keys[i] = i;
        ASSERT_EQ(HopscotchHashTableInsert(ht_ptr, &keys[i]), 0);
    }
    ASSERT_EQ(HopscotchHashTableSize(ht_ptr), keys.size())
        << "the operations did not actually do anything";

    for (int round = 0; round < 200; ++round) {
        for (uint64_t i = 0; i < keys.size(); ++i) {
            (void)HopscotchHashTableLookup(ht_ptr, (const uint8_t *)&keys[i]);
        }
        (void)HopscotchHashTableCapacity(ht_ptr);
    }
    for (uint64_t i = 0; i < keys.size(); ++i) {
        (void)HopscotchHashTableRemove(ht_ptr, (const uint8_t *)&keys[i]);
    }
    (void)HopscotchHashTableIsEmpty(ht_ptr);

    EXPECT_EQ(MockDriverWriteCount(), 0)
        << "a record was emitted from an operational path; logging must stay off it";

    HopscotchHashTableDestroy(ht_ptr);
    UfLoggerDestroy(log_ptr);
}
