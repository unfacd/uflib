/**
 * @file logger_zlog_tests.cpp
 * @brief Tests against the real backend, through the production create path.
 *
 * Where the core suite injects a recording double, this one calls
 * @c UfLoggerCreate and lets the module bind whatever backend it was built
 * with.  That is the only way to exercise @c LoggerBuiltinDriverTable, the
 * configuration text the backend generates, and zlog's own semantics.
 *
 * ## Process-global state
 *
 * The backend's configuration is process-global and single-shot, so test order
 * would otherwise matter.  Each case here creates and destroys its own loggers
 * and is therefore self-contained, with one exception: the case that puts a
 * *foreign* configuration in place runs in a forked child, because it
 * deliberately establishes a state this module did not create and should not
 * leave that state behind for whatever runs next.
 *
 * That one case includes zlog's header directly.  It is the only test that
 * does, and it has to: simulating another owner of zlog is not expressible
 * without naming zlog.  Nothing in the module is permitted to.
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
}

/* Included at file scope, and only for the one case that simulates another
   owner of zlog's process-global state — which is not expressible without
   naming zlog.  Nothing in the module is permitted to include this. */
extern "C" {
#include <uflib/zlog/zlog.h>
}

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char *kFileA = "/tmp/ufsrv_logger_zlog_a.log";
const char *kFileB = "/tmp/ufsrv_logger_zlog_b.log";

std::string ReadFile(const char *path)
{
  FILE *file_ptr = std::fopen(path, "rb");
  if (!file_ptr) return std::string();

  std::string content;
  char        buffer[4096];
  size_t      read_bytes;

  while ((read_bytes = std::fread(buffer, 1, sizeof buffer, file_ptr)) > 0) {
    content.append(buffer, read_bytes);
  }
  std::fclose(file_ptr);
  return content;
}

bool Contains(const char *path, const char *needle)
{
  return ReadFile(path).find(needle) != std::string::npos;
}

UfLoggerConfig FileConfig(const char *category, const char *path, UfLoggerLevel floor)
{
  UfLoggerConfig config;
  std::memset(&config, 0, sizeof config);
  config.category        = category;
  config.minimum_level   = floor;
  config.destination     = UF_LOGGER_DESTINATION_FILE;
  config.file_path       = path;
  config.format_pattern  = "%V|%m%n";
  return config;
}

class LoggerZlog : public ::testing::Test {
protected:
  void SetUp() override
  {
    std::remove(kFileA);
    std::remove(kFileB);
  }
  void TearDown() override
  {
    std::remove(kFileA);
    std::remove(kFileB);
  }
};

}  // namespace

/* ── The production path ────────────────────────────────────────────────── */

TEST_F(LoggerZlog, ModuleBindsItsOwnBackendAndReportsWhatItCanDo)
{
  UfLoggerConfig config = FileConfig("prod", kFileA, UF_LOGGER_LEVEL_INFO);

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(UfLoggerCreate(&config, &log_ptr), UF_LOGGER_STATUS_OK) << "no backend bound";
  ASSERT_NE(log_ptr, nullptr);

  const uint32_t capabilities = UfLoggerGetCapabilities(log_ptr);

  /* The backend that rotates files must say so, or a caller could never ask for
     a rotation size without tripping the capability refusal. */
  EXPECT_TRUE(capabilities & UF_LOGGER_CAP_FILE_ROTATION);
  EXPECT_TRUE(capabilities & UF_LOGGER_CAP_RUNTIME_LEVEL);
  EXPECT_TRUE(capabilities & UF_LOGGER_CAP_RELOAD);
  EXPECT_TRUE(capabilities & UF_LOGGER_CAP_CONTEXT);

  /* And the two it cannot honour it must not claim, so those calls answer
     honestly instead of appearing to succeed. */
  EXPECT_FALSE(capabilities & UF_LOGGER_CAP_FLUSH);
  EXPECT_FALSE(capabilities & UF_LOGGER_CAP_MANUAL_ARCHIVE);
  EXPECT_EQ(UfLoggerFlush(log_ptr), UF_LOGGER_STATUS_ERR_UNSUPPORTED);
  EXPECT_EQ(UfLoggerArchive(log_ptr), UF_LOGGER_STATUS_ERR_UNSUPPORTED);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerZlog, RecordsReachTheFileWithTheConfiguredShape)
{
  UfLoggerConfig config = FileConfig("shape", kFileA, UF_LOGGER_LEVEL_INFO);

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(UfLoggerCreate(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "hello %s", "world"), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UF_LOGGER_WARN(log_ptr, "depth=%zu", static_cast<size_t>(7)), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(log_ptr);

  EXPECT_TRUE(Contains(kFileA, "INFO|hello world"));
  EXPECT_TRUE(Contains(kFileA, "WARN|depth=7"));
}

TEST_F(LoggerZlog, TheFloorSuppressesLowerRanks)
{
  UfLoggerConfig config = FileConfig("floor", kFileA, UF_LOGGER_LEVEL_WARN);

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(UfLoggerCreate(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "below-floor"), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UF_LOGGER_WARN(log_ptr, "at-floor"), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UF_LOGGER_ERROR(log_ptr, "above-floor"), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(log_ptr);

  EXPECT_FALSE(Contains(kFileA, "below-floor"));
  EXPECT_TRUE(Contains(kFileA, "at-floor"));
  EXPECT_TRUE(Contains(kFileA, "above-floor"));
}

/*!
 * Lowering the floor at runtime must actually widen what is delivered.
 *
 * This is a regression test for a real defect: the generated rules originally
 * named the configured severity, and a backend rule's level is a permanent
 * floor of its own that no amount of per-category switching can widen past.  A
 * lowered floor therefore accepted the call, reported success, and dropped
 * every record below the original rule level — a silent no-op, which is the
 * exact failure this module exists to prevent.
 */
TEST_F(LoggerZlog, LoweringTheFloorAtRuntimeWidensWhatIsDelivered)
{
  UfLoggerConfig config = FileConfig("widen", kFileA, UF_LOGGER_LEVEL_INFO);

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(UfLoggerCreate(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  ASSERT_EQ(UF_LOGGER_DEBUG(log_ptr, "before-widening"), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UfLoggerSetLevel(log_ptr, UF_LOGGER_LEVEL_DEBUG), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UF_LOGGER_DEBUG(log_ptr, "after-widening"), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(log_ptr);

  EXPECT_FALSE(Contains(kFileA, "before-widening"));
  EXPECT_TRUE(Contains(kFileA, "after-widening")) << "runtime level change was a silent no-op";
}

TEST_F(LoggerZlog, TwoLoggersKeepTheirOwnCategoriesAndDestinations)
{
  UfLoggerConfig first  = FileConfig("alpha", kFileA, UF_LOGGER_LEVEL_INFO);
  UfLoggerConfig second = FileConfig("beta", kFileB, UF_LOGGER_LEVEL_WARN);

  UfLogger *a = nullptr;
  UfLogger *b = nullptr;
  ASSERT_EQ(UfLoggerCreate(&first, &a), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UfLoggerCreate(&second, &b), UF_LOGGER_STATUS_OK);

  ASSERT_EQ(UF_LOGGER_INFO(a, "alpha-line"), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UF_LOGGER_INFO(b, "beta-suppressed"), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UF_LOGGER_ERROR(b, "beta-line"), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(b);
  UfLoggerDestroy(a);

  EXPECT_TRUE(Contains(kFileA, "alpha-line"));
  EXPECT_FALSE(Contains(kFileA, "beta-line"));
  EXPECT_FALSE(Contains(kFileB, "beta-suppressed"));
  EXPECT_TRUE(Contains(kFileB, "beta-line"));
}

TEST_F(LoggerZlog, ReloadChangesTheFloor)
{
  UfLoggerConfig config = FileConfig("reloaded", kFileA, UF_LOGGER_LEVEL_INFO);

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(UfLoggerCreate(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  UfLoggerConfig raised = FileConfig("reloaded", kFileA, UF_LOGGER_LEVEL_ERROR);
  ASSERT_EQ(UfLoggerReload(log_ptr, &raised), UF_LOGGER_STATUS_OK);
  EXPECT_EQ(UfLoggerGetLevel(log_ptr), UF_LOGGER_LEVEL_ERROR);

  ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "dropped-after-reload"), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UF_LOGGER_ERROR(log_ptr, "kept-after-reload"), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(log_ptr);

  EXPECT_FALSE(Contains(kFileA, "dropped-after-reload"));
  EXPECT_TRUE(Contains(kFileA, "kept-after-reload"));
}

TEST_F(LoggerZlog, ContextReachesThePattern)
{
  UfLoggerConfig config = FileConfig("context", kFileA, UF_LOGGER_LEVEL_INFO);
  config.format_pattern = "%V|%M(session)|%m%n";

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(UfLoggerCreate(&config, &log_ptr), UF_LOGGER_STATUS_OK);

  ASSERT_EQ(UfLoggerSetContext(log_ptr, "session", "S-42"), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "with-context"), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(log_ptr);

  EXPECT_TRUE(Contains(kFileA, "INFO|S-42|with-context"));
}

TEST_F(LoggerZlog, AConfiguredRotationSizeIsAccepted)
{
  UfLoggerConfig config = FileConfig("rotating", kFileA, UF_LOGGER_LEVEL_INFO);
  config.max_file_bytes = 65536u;
  config.max_archives   = 3u;

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(UfLoggerCreate(&config, &log_ptr), UF_LOGGER_STATUS_OK);
  ASSERT_EQ(UF_LOGGER_INFO(log_ptr, "rotating-line"), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(log_ptr);

  EXPECT_TRUE(Contains(kFileA, "rotating-line"));
}

TEST_F(LoggerZlog, AZeroedConfigurationIsValid)
{
  UfLoggerConfig config;
  std::memset(&config, 0, sizeof config);

  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(UfLoggerCreate(&config, &log_ptr), UF_LOGGER_STATUS_OK);
  ASSERT_NE(log_ptr, nullptr);

  /* Every field unset must resolve to the module's default rather than to
     whatever zero happens to mean for that field. */
  EXPECT_EQ(UfLoggerGetLevel(log_ptr), UF_LOGGER_LEVEL_INFO);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerZlog, CreateWithDefaultsWorks)
{
  UfLogger *log_ptr = nullptr;
  ASSERT_EQ(UfLoggerCreateWithDefaults(&log_ptr), UF_LOGGER_STATUS_OK);
  ASSERT_NE(log_ptr, nullptr);

  /* Default destination is syslog; the record still has to be accepted. */
  EXPECT_EQ(UF_LOGGER_INFO(log_ptr, "to-syslog"), UF_LOGGER_STATUS_OK);

  UfLoggerDestroy(log_ptr);
}

TEST_F(LoggerZlog, ManyLoggersThenAllReleased)
{
  /* Creation regenerates the shared configuration each time, which is the path
     most likely to fail silently if the generated text were ever malformed. */
  enum { kCount = 6 };
  UfLogger *loggers[kCount];

  for (int i = 0; i < kCount; i++) {
    char name[32];
    std::snprintf(name, sizeof name, "many_%d", i);

    UfLoggerConfig config = FileConfig("many", kFileA, UF_LOGGER_LEVEL_INFO);
    config.category = name;

    loggers[i] = nullptr;
    ASSERT_EQ(UfLoggerCreate(&config, &loggers[i]), UF_LOGGER_STATUS_OK) << "logger " << i;
    ASSERT_NE(loggers[i], nullptr);
  }

  for (int i = 0; i < kCount; i++) {
    ASSERT_EQ(UF_LOGGER_INFO(loggers[i], "from-%d", i), UF_LOGGER_STATUS_OK);
  }
  for (int i = 0; i < kCount; i++) {
    UfLoggerDestroy(loggers[i]);
  }

  for (int i = 0; i < kCount; i++) {
    char expected[32];
    std::snprintf(expected, sizeof expected, "from-%d", i);
    EXPECT_TRUE(Contains(kFileA, expected)) << "record " << i;
  }
}

/* ── Stream destinations ────────────────────────────────────────────────── */

/*!
 * Both stream destinations, each in a forked child.
 *
 * The child redirects the stream it is testing and asserts the record landed
 * there; the parent asserts the child's verdict.  Forking is what makes the
 * redirection safe — doing it in-process would leave the suite's own output
 * pointing at a file for everything that ran afterwards.
 */
TEST_F(LoggerZlog, StreamDestinationsReachTheirOwnStream)
{
  struct Case {
    UfLoggerDestination destination;
    int                 fd_number;
    const char         *path;
  };

  const Case cases[] = {
    { UF_LOGGER_DESTINATION_STDOUT, 1, "/tmp/ufsrv_logger_zlog_stdout.log" },
    { UF_LOGGER_DESTINATION_STDERR, 2, "/tmp/ufsrv_logger_zlog_stderr.log" },
  };

  for (const Case &test_case : cases) {
    std::remove(test_case.path);

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
      if (!std::freopen(test_case.path, "w", test_case.fd_number == 1 ? stdout : stderr)) _exit(20);

      UfLoggerConfig config;
      std::memset(&config, 0, sizeof config);
      config.category        = "stream";
      config.minimum_level   = UF_LOGGER_LEVEL_INFO;
      config.destination     = test_case.destination;
      config.format_pattern  = "%V|%m%n";

      UfLogger *log_ptr = nullptr;
      if (UfLoggerCreate(&config, &log_ptr) != UF_LOGGER_STATUS_OK || !log_ptr) _exit(21);

      if (UF_LOGGER_WARN(log_ptr, "stream-line") != UF_LOGGER_STATUS_OK) _exit(22);

      UfLoggerDestroy(log_ptr);
      std::fflush(nullptr);
      _exit(0);
    }

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status)) << test_case.path;
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << test_case.path << ": 20 = redirect failed, 21 = create failed, 22 = emit failed";
    EXPECT_TRUE(Contains(test_case.path, "WARN|stream-line")) << test_case.path;

    std::remove(test_case.path);
  }
}

/* ── A foreign owner of the process-global state ────────────────────────── */

TEST_F(LoggerZlog, AForeignOwnerIsRefusedRatherThanDisplaced)
{
  /* Forked: the child establishes a configuration this module did not create,
     and the assertion is about what the module does in that situation.  Running
     it in-process would make every later case depend on the child's cleanup. */
  pid_t child = fork();
  ASSERT_NE(child, -1);

  if (child == 0) {
    /* The child must not run gtest's teardown; it exists only to produce an
       exit status. */
    int result;
    {
      /* Someone else — another library in the same process — got here first. */
      if (zlog_init_from_string("[global]\nstrict init = false\n[rules]\nfoe.* >stdout\n") != 0) {
        _exit(10);
      }

      UfLoggerConfig config = FileConfig("mine", kFileA, UF_LOGGER_LEVEL_INFO);
      UfLogger *log_ptr = nullptr;

      /* Taking the configuration by force would silently discard the other
         owner's rules, so the module must decline instead. */
      result = (UfLoggerCreate(&config, &log_ptr) == UF_LOGGER_STATUS_ERR_BUSY &&
                log_ptr == nullptr) ? 0 : 1;

      zlog_fini();

      /* With the other owner gone, the module must be able to take it. */
      if (result == 0) {
        UfLogger *second_ptr = nullptr;
        if (UfLoggerCreate(&config, &second_ptr) != UF_LOGGER_STATUS_OK || !second_ptr) {
          result = 2;
        } else {
          UfLoggerDestroy(second_ptr);
        }
      }
    }
    _exit(result);
  }

  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status)) << "child did not exit normally";
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "0 expected; 1 = foreign owner was displaced instead of refused; "
         "2 = module could not take the state after the owner released it; "
         "10 = zlog could not be initialised in the child";
}
