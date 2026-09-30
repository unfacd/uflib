/**
 * @file logger_core_tests.cpp
 * @brief Adversarial tests for the logger core, against a recording double.
 *
 * These exercise the module's contract by trying to break it: inputs at and
 * past every boundary, format strings that would be interpreted if anything
 * were careless, lifecycles run out of order, and configurations that ask for
 * what the bound backend cannot do.
 *
 * The double is reached through @c mock_driver.h, which is deliberately the
 * only thing here that knows a driver table exists.  Tests observe through the
 * public interface — including @c UfLoggerDescribeConfiguration, which is how
 * the compiled model is inspected without reaching into the module.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "gtest/gtest.h"

extern "C" {
#include <uflib/logger/logger.h>
#include "mock_driver.h"
}

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

/* A configuration naming only what the test cares about; every other field
   zeroed so the module's own default applies. */
UfLoggerConfig BaseConfig()
{
  UfLoggerConfig config;
  std::memset(&config, 0, sizeof config);
  config.category      = "core";
  config.minimum_level = UF_LOGGER_LEVEL_INFO;
  config.destination   = UF_LOGGER_DESTINATION_SYSLOG;
  return config;
}

/* Emitting from a named function, so a test can assert the caller's identity
   survived rather than the logger's. */
void EmitFromNamedHelper(UfLogger *log_ptr)
{
  UF_LOGGER_INFO(log_ptr, "from-helper");
}

std::string Describe(UfLogger *log_ptr)
{
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 512);
  UfLoggerDescribeConfiguration(log_ptr, &bd);
  std::string text(bd.data ? bd.data : "");
  BufferDescriptorRelease(&bd);
  return text;
}

class LoggerCore : public ::testing::Test {
protected:
  void SetUp() override { MockDriverReset(); }
};

}  // namespace

/* ── Defaults and construction ──────────────────────────────────────────── */

TEST_F(LoggerCore, SaneDefaultsAreUsableAsIs)
{
  const UfLoggerConfig *defaults_ptr = UfLoggerProvideSaneDefaults();
  ASSERT_NE(defaults_ptr, nullptr);
  EXPECT_NE(defaults_ptr->category, nullptr);
  EXPECT_EQ(defaults_ptr->minimum_level, UF_LOGGER_LEVEL_INFO);
  EXPECT_EQ(defaults_ptr->destination, UF_LOGGER_DESTINATION_SYSLOG);

  UfLogger *log_ptr = nullptr;
  EXPECT_EQ(MockDriverCreateLogger(defaults_ptr, &log_ptr), UF_LOGGER_STATUS_OK);
  EXPECT_NE(log_ptr, nullptr);
  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, SaneDefaultsCanBeCopiedAndChanged)
{
  UfLoggerConfig config = *UfLoggerProvideSaneDefaults();
  config.category      = "copied";
  config.minimum_level = UF_LOGGER_LEVEL_DEBUG;

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UfLoggerGetLevel(log_ptr), UF_LOGGER_LEVEL_DEBUG);
  EXPECT_NE(Describe(log_ptr).find("\"category\":\"copied\""), std::string::npos);
  UfLoggerDestroy(log_ptr);

  /* The shared default must not have been written through. */
  EXPECT_STREQ(UfLoggerProvideSaneDefaults()->category, "uflib");
}

TEST_F(LoggerCore, NullOutputPointerIsRefused)
{
  UfLoggerConfig config = BaseConfig();
  EXPECT_EQ(MockDriverCreateLogger(&config, nullptr), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_F(LoggerCore, NullConfigIsRefusedAndClearsOutput)
{
  UfLogger *log_ptr = reinterpret_cast<UfLogger *>(0x1);
  EXPECT_EQ(MockDriverCreateLogger(nullptr, &log_ptr), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(log_ptr, nullptr);
}

TEST_F(LoggerCore, BackendOpenFailurePropagatesAndLeavesNothingBehind)
{
  MockDriverFailNextOpen(UF_LOGGER_STATUS_ERR_IO);
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;

  EXPECT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_ERR_IO);
  EXPECT_EQ(log_ptr, nullptr);
  EXPECT_EQ(MockDriverCloseCount(), 0);   /* nothing was opened, so nothing closed */
}

/* ── Severity ───────────────────────────────────────────────────────────── */

TEST_F(LoggerCore, RecordsBelowTheFloorCostNothing)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  EXPECT_EQ(UF_LOGGER_DEBUG(log_ptr, "suppressed %d", 1), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(MockDriverWriteCount(), 0);
  EXPECT_EQ(MockDriverRecordCount(), 0);

  EXPECT_EQ(UF_LOGGER_INFO(log_ptr, "delivered"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(MockDriverWriteCount(), 1);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, EveryRankCanBeEmittedAtTheLowestFloor)
{
  UfLoggerConfig config = BaseConfig();
  config.minimum_level = UF_LOGGER_LEVEL_TRACE;
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  EXPECT_EQ(UF_LOGGER_TRACE(log_ptr, "t"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UF_LOGGER_DEBUG(log_ptr, "d"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UF_LOGGER_INFO(log_ptr, "i"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UF_LOGGER_NOTICE(log_ptr, "n"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UF_LOGGER_WARN(log_ptr, "w"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UF_LOGGER_ERROR(log_ptr, "e"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UF_LOGGER_CRITICAL(log_ptr, "c"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UF_LOGGER_FATAL(log_ptr, "f"), UF_LOGGER_STATUS_OK);

  for (unsigned i = UF_LOGGER_LEVEL_TRACE; i <= UF_LOGGER_LEVEL_FATAL; i++) {
    EXPECT_EQ(MockDriverWriteCountForLevel(static_cast<UfLoggerLevel>(i)), 1) << "rank " << i;
  }
  EXPECT_EQ(MockDriverWriteCount(), 8);
  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, DefaultRankIsNotALevel)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  /* The 0 rank means "unspecified", so emitting at it is a caller error rather
     than a request for the quietest possible record. */
  EXPECT_EQ(UfLoggerLog(log_ptr, UF_LOGGER_LEVEL_DEFAULT, __FILE__, __LINE__, __func__, "x"),
            UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerSetLevel(log_ptr, UF_LOGGER_LEVEL_DEFAULT),
            UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(MockDriverWriteCount(), 0);
  EXPECT_EQ(MockDriverLevelSetCount(), 0);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, RuntimeLevelChangeTakesEffect)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  EXPECT_FALSE(UfLoggerIsLevelEnabled(log_ptr, UF_LOGGER_LEVEL_DEBUG));
  ASSERT_EQ(UfLoggerSetLevel(log_ptr, UF_LOGGER_LEVEL_DEBUG), UF_LOGGER_STATUS_OK);
  EXPECT_TRUE(UfLoggerIsLevelEnabled(log_ptr, UF_LOGGER_LEVEL_DEBUG));
  EXPECT_EQ(MockDriverLastLevelSet(), UF_LOGGER_LEVEL_DEBUG);
  EXPECT_EQ(UF_LOGGER_DEBUG(log_ptr, "now-visible"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(MockDriverWriteCount(), 1);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, NullLoggerReportsTheQuietestLevel)
{
  EXPECT_EQ(UfLoggerGetLevel(nullptr), UF_LOGGER_LEVEL_FATAL);
  EXPECT_FALSE(UfLoggerIsLevelEnabled(nullptr, UF_LOGGER_LEVEL_TRACE));
  EXPECT_FALSE(UfLoggerIsLevelEnabled(nullptr, UF_LOGGER_LEVEL_DEFAULT));
}

/* ── Formatting and message content ─────────────────────────────────────── */

TEST_F(LoggerCore, CallerSourceLocationSurvives)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  EmitFromNamedHelper(log_ptr);

  const MockDriverRecord *record_ptr = MockDriverLastRecord();
  ASSERT_NE(record_ptr, nullptr);
  EXPECT_EQ(record_ptr->level, UF_LOGGER_LEVEL_INFO);
  EXPECT_STREQ(record_ptr->message, "from-helper");
  EXPECT_NE(std::string(record_ptr->file).find("logger_core_tests.cpp"), std::string::npos);
  EXPECT_STREQ(record_ptr->function, "EmitFromNamedHelper");
  EXPECT_GT(record_ptr->line, 0);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, FormatStringInTheMessageIsNotInterpreted)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  /* A caller that logs untrusted text must not be able to turn it into a format
     string.  If the message were passed through as a format, this would read
     past its arguments. */
  const char *hostile = "%s %n %999999d %x %%";
  ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "%s", hostile), UF_LOGGER_STATUS_OK);

  const MockDriverRecord *record_ptr = MockDriverLastRecord();
  ASSERT_NE(record_ptr, nullptr);
  EXPECT_STREQ(record_ptr->message, hostile);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, MessageLongerThanTheStackBufferArrivesWhole)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  /* Comfortably past the module's 1 KiB stack buffer, so this takes the
     allocation path, and comfortably inside the double's capture, so a
     truncation would be the module's doing rather than the test's. */
  std::string long_message(1500, 'z');
  ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "%s", long_message.c_str()), UF_LOGGER_STATUS_OK);

  const MockDriverRecord *record_ptr = MockDriverLastRecord();
  ASSERT_NE(record_ptr, nullptr);
  EXPECT_EQ(std::strlen(record_ptr->message), 1500u);
  EXPECT_EQ(record_ptr->message[0], 'z');
  EXPECT_EQ(record_ptr->message[1499], 'z');
  EXPECT_EQ(MockDriverWriteCount(), 1);   /* truncated or not, never dropped */

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, EmptyFormatIsAccepted)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  EXPECT_EQ(UF_LOGGER_INFO(log_ptr, "%s", ""), UF_LOGGER_STATUS_OK);
  const MockDriverRecord *record_ptr = MockDriverLastRecord();
  ASSERT_NE(record_ptr, nullptr);
  EXPECT_STREQ(record_ptr->message, "");

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, NullFormatIsRefused)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  EXPECT_EQ(UfLoggerLog(log_ptr, UF_LOGGER_LEVEL_INFO, __FILE__, __LINE__, __func__, nullptr),
            UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(MockDriverWriteCount(), 0);

  UfLoggerDestroy(log_ptr);
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

TEST_F(LoggerCore, DestroyClosesTheBackendExactlyOnce)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(log_ptr);
  EXPECT_EQ(MockDriverCloseCount(), 1);

  /* A second destroy of the same pointer is undefined by contract and is not
     exercised here; destroying NULL is defined and must do nothing. */
  UfLoggerDestroy(nullptr);
  EXPECT_EQ(MockDriverCloseCount(), 1);
}

/*!
 * The one lifetime question the library can answer: destroying NULL.
 *
 * This case was previously named `EmittingAfterDestroyIsRefusedNotFatal` and
 * claimed, in its name and its comment, to check that emitting through a
 * destroyed handle is answered with a status.  It never emitted through one, so
 * the property it advertised was untested — and untestable: `UfLoggerDestroy`
 * frees the handle, so any such call reads freed memory.  Asserting on it would
 * be testing undefined behaviour, passing or failing on where the allocator
 * happened to place things, and ASan flagging the read is the correct outcome
 * rather than a test failure.
 *
 * The contract therefore states the obligation instead of offering a status
 * code for it — see the lifetime note in logger.h.  What remains testable is
 * the NULL no-op, which is what this case covers.
 */
TEST_F(LoggerCore, DestroyNullIsSafe)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(log_ptr);
  EXPECT_EQ(MockDriverCloseCount(), 1);

  /* Unrelated to the handle above: this is the guard that makes shutdown code
     safe to write when a logger may never have been created. */
  UfLoggerDestroy(nullptr);
  EXPECT_EQ(MockDriverCloseCount(), 1);
}

TEST_F(LoggerCore, DestroyingOneLoggerLeavesItsSiblingWorking)
{
  UfLoggerConfig first = BaseConfig();
  UfLoggerConfig second = BaseConfig();
  second.category = "sibling";

  UfLogger *a = nullptr;
  UfLogger *b = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&first, &a), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(MockDriverCreateLogger(&second, &b), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(a);

  EXPECT_EQ(UF_LOGGER_INFO(b, "still-here"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(MockDriverWriteCount(), 1);
  EXPECT_EQ(MockDriverCloseCount(), 1);

  UfLoggerDestroy(b);
  EXPECT_EQ(MockDriverCloseCount(), 2);
}

TEST_F(LoggerCore, NullLoggerIsSafeAtEveryEntryPoint)
{
  UfLogger *null_log = nullptr;

  EXPECT_EQ(UF_LOGGER_INFO(null_log, "x"), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerFlush(null_log), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerArchive(null_log), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerReload(null_log, nullptr), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerSetContext(null_log, "k", "v"), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerRemoveContext(null_log, "k"), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerClearContext(null_log), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerSetLevel(null_log, UF_LOGGER_LEVEL_INFO), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerGetCapabilities(null_log), 0u);
  UfLoggerDestroy(null_log);
}

/* ── Capabilities ───────────────────────────────────────────────────────── */

TEST_F(LoggerCore, UndeclaredOperationsAreRefusedRatherThanIgnored)
{
  MockDriverSetCapabilities(0u);

  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  EXPECT_EQ(UfLoggerGetCapabilities(log_ptr), 0u);
  EXPECT_EQ(UfLoggerFlush(log_ptr), UF_LOGGER_STATUS_ERR_UNSUPPORTED);
  EXPECT_EQ(UfLoggerArchive(log_ptr), UF_LOGGER_STATUS_ERR_UNSUPPORTED);
  EXPECT_EQ(UfLoggerReload(log_ptr, nullptr), UF_LOGGER_STATUS_ERR_UNSUPPORTED);
  EXPECT_EQ(UfLoggerSetLevel(log_ptr, UF_LOGGER_LEVEL_DEBUG), UF_LOGGER_STATUS_ERR_UNSUPPORTED);
  EXPECT_EQ(UfLoggerSetContext(log_ptr, "k", "v"), UF_LOGGER_STATUS_ERR_UNSUPPORTED);

  /* Refused, not performed: none of the backend slots may have been reached. */
  EXPECT_EQ(MockDriverFlushCount(), 0);
  EXPECT_EQ(MockDriverArchiveCount(), 0);
  EXPECT_EQ(MockDriverReloadCount(), 0);
  EXPECT_EQ(MockDriverLevelSetCount(), 0);
  EXPECT_EQ(MockDriverContextSetCount(), 0);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, DeclaredOperationsReachTheBackend)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  EXPECT_EQ(UfLoggerFlush(log_ptr), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UfLoggerArchive(log_ptr), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UfLoggerSetContext(log_ptr, "k", "v"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UfLoggerRemoveContext(log_ptr, "k"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UfLoggerClearContext(log_ptr), UF_LOGGER_STATUS_OK);

  EXPECT_EQ(MockDriverFlushCount(), 1);
  EXPECT_EQ(MockDriverArchiveCount(), 1);
  EXPECT_EQ(MockDriverContextSetCount(), 1);
  EXPECT_EQ(MockDriverContextRemoveCount(), 1);
  EXPECT_EQ(MockDriverContextClearCount(), 1);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, ExplicitRotationAgainstABackendWithoutItIsRefused)
{
  MockDriverSetCapabilities(UF_LOGGER_CAP_RUNTIME_LEVEL);   /* no FILE_ROTATION */

  UfLoggerConfig config = BaseConfig();
  config.destination    = UF_LOGGER_DESTINATION_FILE;
  config.file_path      = "/tmp/never-written.log";
  config.max_file_bytes = 4096u;

  UfLogger *log_ptr = nullptr;
  EXPECT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_ERR_CAPABILITY);
  EXPECT_EQ(log_ptr, nullptr);
}

TEST_F(LoggerCore, DefaultedRotationAgainstABackendWithoutItIsNotAnError)
{
  MockDriverSetCapabilities(0u);

  /* The caller expressed no preference, so falling back to no rotation loses
     nothing it asked for — unlike the explicit case above. */
  UfLoggerConfig config = BaseConfig();
  config.destination = UF_LOGGER_DESTINATION_FILE;
  config.file_path   = "/tmp/never-written.log";

  UfLogger *log_ptr = nullptr;
  EXPECT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);
  EXPECT_NE(log_ptr, nullptr);
  UfLoggerDestroy(log_ptr);
}

/* ── Configuration boundaries ───────────────────────────────────────────── */

TEST_F(LoggerCore, FileDestinationRequiresAPath)
{
  UfLoggerConfig missing = BaseConfig();
  missing.destination = UF_LOGGER_DESTINATION_FILE;
  missing.file_path   = nullptr;

  UfLogger *log_ptr = nullptr;
  EXPECT_EQ(MockDriverCreateLogger(&missing, &log_ptr), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);

  UfLoggerConfig empty = BaseConfig();
  empty.destination = UF_LOGGER_DESTINATION_FILE;
  empty.file_path   = "";

  EXPECT_EQ(MockDriverCreateLogger(&empty, &log_ptr), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(log_ptr, nullptr);
}

TEST_F(LoggerCore, CategoryLengthBoundary)
{
  std::string at_limit(1023, 'c');    /* one short of the ceiling */
  std::string past_limit(1024, 'c');

  UfLoggerConfig config = BaseConfig();
  config.category = at_limit.c_str();

  UfLogger *log_ptr = nullptr;
  EXPECT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);
  UfLoggerDestroy(log_ptr);

  config.category = past_limit.c_str();
  EXPECT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(log_ptr, nullptr);
}

TEST_F(LoggerCore, EmptyCategoryFallsBackToTheDefault)
{
  UfLoggerConfig config = BaseConfig();
  config.category = "";

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);
  EXPECT_NE(Describe(log_ptr).find("uflib"), std::string::npos);
  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, ContextLengthBoundaries)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  std::string key_at_limit(63, 'k');
  std::string key_past_limit(64, 'k');
  std::string value_at_limit(255, 'v');
  std::string value_past_limit(256, 'v');

  EXPECT_EQ(UfLoggerSetContext(log_ptr, key_at_limit.c_str(), value_at_limit.c_str()),
            UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UfLoggerSetContext(log_ptr, key_past_limit.c_str(), "v"),
            UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerSetContext(log_ptr, "k", value_past_limit.c_str()),
            UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerSetContext(log_ptr, "", "v"), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(UfLoggerSetContext(log_ptr, "k", nullptr), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);

  EXPECT_EQ(MockDriverContextSetCount(), 1);
  UfLoggerDestroy(log_ptr);
}

/* ── Reload ─────────────────────────────────────────────────────────────── */

TEST_F(LoggerCore, ReloadAppliesANewConfiguration)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  UfLoggerConfig raised = BaseConfig();
  raised.minimum_level = UF_LOGGER_LEVEL_ERROR;

  ASSERT_EQ(UfLoggerReload(log_ptr, &raised), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UfLoggerGetLevel(log_ptr), UF_LOGGER_LEVEL_ERROR);
  EXPECT_EQ(MockDriverReloadCount(), 1);

  EXPECT_EQ(UF_LOGGER_INFO(log_ptr, "dropped"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UF_LOGGER_ERROR(log_ptr, "kept"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(MockDriverWriteCount(), 1);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, AFailedReloadLeavesTheLoggerUnchanged)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  /* A FILE destination with no path cannot be carried out.  A reload that
     cannot be applied must not leave a half-configured logger behind. */
  UfLoggerConfig broken = BaseConfig();
  broken.destination = UF_LOGGER_DESTINATION_FILE;
  broken.file_path   = nullptr;

  EXPECT_EQ(UfLoggerReload(log_ptr, &broken), UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(MockDriverReloadCount(), 0);
  EXPECT_EQ(UfLoggerGetLevel(log_ptr), UF_LOGGER_LEVEL_INFO);

  EXPECT_EQ(UF_LOGGER_INFO(log_ptr, "still-working"), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(MockDriverWriteCount(), 1);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, ReloadWithNoConfigurationReappliesTheCurrentOne)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  EXPECT_EQ(UfLoggerReload(log_ptr, nullptr), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(MockDriverReloadCount(), 1);
  EXPECT_EQ(UfLoggerGetLevel(log_ptr), UF_LOGGER_LEVEL_INFO);

  UfLoggerDestroy(log_ptr);
}

/* ── Description ────────────────────────────────────────────────────────── */

TEST_F(LoggerCore, DescriptionReportsTheEffectiveConfiguration)
{
  UfLoggerConfig config = BaseConfig();
  config.minimum_level = UF_LOGGER_LEVEL_WARN;
  config.file_path     = "/tmp/described.log";
  config.destination   = UF_LOGGER_DESTINATION_FILE;
  config.max_archives  = 3u;

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  std::string json = Describe(log_ptr);
  ASSERT_FALSE(json.empty());
  EXPECT_EQ(json.front(), '{');
  EXPECT_EQ(json.back(), '\n');

  EXPECT_NE(json.find("\"category\":\"core\""), std::string::npos);
  EXPECT_NE(json.find("\"minimum_level\":\"WARN\""), std::string::npos);
  EXPECT_NE(json.find("\"destination\":\"file\""), std::string::npos);
  EXPECT_NE(json.find("\"file_path\":\"/tmp/described.log\""), std::string::npos);
  EXPECT_NE(json.find("\"max_archives\":3"), std::string::npos);
  EXPECT_NE(json.find("\"capabilities\""), std::string::npos);
  EXPECT_NE(json.find("\"runtime_level\""), std::string::npos);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, DescriptionOmitsRotationForANonFileDestination)
{
  UfLoggerConfig config = BaseConfig();   /* syslog */
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  /* Reporting a rotation size against a destination that has no file would
     describe something that is not happening. */
  std::string json = Describe(log_ptr);
  EXPECT_NE(json.find("\"max_file_bytes\":null"), std::string::npos);
  EXPECT_NE(json.find("\"max_archives\":null"), std::string::npos);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, DescriptionOfANullHandleIsAnObjectNotACrash)
{
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(UfLoggerDescribeConfiguration(nullptr, &bd), &bd);
  ASSERT_NE(bd.data, nullptr);
  EXPECT_NE(std::string(bd.data).find("\"error\""), std::string::npos);
  BufferDescriptorRelease(&bd);
}

TEST_F(LoggerCore, DescriptionAllocatesWhenGivenNothing)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  BufferDescriptor *bd_ptr = UfLoggerDescribeConfiguration(log_ptr, nullptr);
  ASSERT_NE(bd_ptr, nullptr);
  ASSERT_NE(bd_ptr->data, nullptr);
  BufferDescriptorRelease(bd_ptr);
  std::free(bd_ptr);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerCore, DescriptionAppendsRatherThanResetting)
{
  UfLoggerConfig config = BaseConfig();
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(MockDriverCreateLogger(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  ASSERT_EQ(BufferDescriptorAppendFormatted(&bd, "PREFIX:"), BUFFER_DESCRIPTOR_OK);

  UfLoggerDescribeConfiguration(log_ptr, &bd);
  ASSERT_NE(bd.data, nullptr);
  EXPECT_EQ(std::strncmp(bd.data, "PREFIX:{", 8), 0);

  BufferDescriptorRelease(&bd);
  UfLoggerDestroy(log_ptr);
}
