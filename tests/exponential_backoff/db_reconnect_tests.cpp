/**
 * @file db_reconnect_tests.cpp
 * @brief Whitebox test of the DB connection-loss retry loop in h_execute_query.
 *
 * h_execute_query (src/db/db_abstraction.c) is hardwired to call
 * h_execute_query_mariadb and h_reconnect_mariadb, so a real MariaDB server is
 * normally required to exercise the retry path.  This harness substitutes those
 * two I/O calls with scripted mocks via the GNU linker's `--wrap` option
 * (see tests/exponential_backoff/CMakeLists.txt) and supplies a *recording*
 * on_sleep provider, so the real retry loop runs against a simulated
 * "stalling" connection:
 *
 *   - the query mock returns H_ERROR_CONNECTION to simulate a dropped/lagged
 *     connection;
 *   - the reconnect mock fails/succeeds on a scripted schedule;
 *   - the backoff delay between attempts is captured (not slept) by the
 *     recording on_sleep, so the exponential growth can be asserted without a
 *     multi-second wall-clock test.
 *
 * Scenarios: immediate success, immediate reconnect recovery, backoff-with-
 * recovery, and full retry exhaustion (which also locks in the off-by-one fix —
 * exactly maxAttempts reconnect attempts, not maxAttempts + 1).
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "gtest/gtest.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include <uflib/db/db_sql.h>
#include <uflib/exponential_backoff/algorithm.h>
}

namespace {

// Mock DB-I/O state, reset before each test.
int          g_query_results[16];
size_t       g_query_results_len = 0;
size_t       g_query_calls = 0;
unsigned int g_reconnect_results[16];
size_t       g_reconnect_results_len = 0;
size_t       g_reconnect_calls = 0;
long         g_sleep_delays[16];
size_t       g_sleep_calls = 0;

void
ResetMocks()
{
  memset(g_query_results, 0, sizeof(g_query_results));
  g_query_results_len = 0;
  g_query_calls = 0;
  memset(g_reconnect_results, 0, sizeof(g_reconnect_results));
  g_reconnect_results_len = 0;
  g_reconnect_calls = 0;
  memset(g_sleep_delays, 0, sizeof(g_sleep_delays));
  g_sleep_calls = 0;
}

}  // namespace

// C-linkage so the linker --wrap substitution and the on_sleep_provider
// typedef match exactly.
extern "C" {
static int
RecordSleep(long delay_ms)
{
  if (g_sleep_calls < 16) {
    g_sleep_delays[g_sleep_calls++] = delay_ms;
  }
  return 0;
}

int
__wrap_h_execute_query_mariadb(struct _h_connection *conn, const char *query,
                               struct _h_result *result)
{
  (void)conn; (void)query; (void)result;
  int r = H_OK;
  if (g_query_results_len > 0) {
    r = (g_query_calls < g_query_results_len)
            ? g_query_results[g_query_calls]
            : g_query_results[g_query_results_len - 1];
  }
  g_query_calls++;
  return r;
}

unsigned int
__wrap_h_reconnect_mariadb(struct _h_connection *conn_db)
{
  (void)conn_db;
  unsigned int r = 0;
  if (g_reconnect_results_len > 0) {
    r = (g_reconnect_calls < g_reconnect_results_len)
            ? g_reconnect_results[g_reconnect_calls]
            : g_reconnect_results[g_reconnect_results_len - 1];
  }
  g_reconnect_calls++;
  return r;
}
}  // extern "C"

namespace {

// Build a MARIADB connection whose backoff descriptor uses the exponential
// strategy (base 1000 ms, max 10000 ms, 5 attempts) and whose on_sleep records
// rather than sleeps.  `connection` is a dummy pointer — the mocks never touch it.
void
MakeConnection(struct _h_connection *conn)
{
  static int dummy_connection = 0;

  memset(conn, 0, sizeof(*conn));
  conn->type = HOEL_DB_TYPE_MARIADB;
  conn->connection = &dummy_connection;
  BackoffAlgorithm_InitializeParamsExponential(
      &conn->backoff_descriptor.retry_params, 1000, 10000, 5);
  conn->backoff_descriptor.on_sleep = RecordSleep;
}

}  // namespace

// ── Scenarios ──────────────────────────────────────────────────────────────

TEST(DbReconnect, QuerySucceedsImmediately)
{
  ResetMocks();
  g_query_results[0] = H_OK; g_query_results_len = 1;

  struct _h_connection conn;
  MakeConnection(&conn);

  EXPECT_EQ(h_execute_query(&conn, "SELECT 1", nullptr, 0), H_OK);
  EXPECT_EQ(g_query_calls, 1u);
  EXPECT_EQ(g_reconnect_calls, 0u);
  EXPECT_EQ(g_sleep_calls, 0u);
}

TEST(DbReconnect, ImmediateReconnectRecovers)
{
  ResetMocks();
  g_query_results[0] = H_ERROR_CONNECTION;
  g_query_results[1] = H_OK;
  g_query_results_len = 2;
  g_reconnect_results[0] = 0;          // immediate reconnect succeeds
  g_reconnect_results_len = 1;

  struct _h_connection conn;
  MakeConnection(&conn);

  EXPECT_EQ(h_execute_query(&conn, "SELECT 1", nullptr, 0), H_OK);
  EXPECT_EQ(g_query_calls, 2u);        // first fails, retry succeeds
  EXPECT_EQ(g_reconnect_calls, 1u);    // one immediate reconnect
  EXPECT_EQ(g_sleep_calls, 0u);        // no backoff needed
}

TEST(DbReconnect, BackoffRetriesThenRecovers)
{
  ResetMocks();
  g_query_results[0] = H_ERROR_CONNECTION;
  g_query_results[1] = H_OK;
  g_query_results_len = 2;
  // immediate reconnect fails, then two backoff reconnects fail, third succeeds.
  g_reconnect_results[0] = 1;
  g_reconnect_results[1] = 1;
  g_reconnect_results[2] = 1;
  g_reconnect_results[3] = 0;
  g_reconnect_results_len = 4;

  struct _h_connection conn;
  MakeConnection(&conn);

  EXPECT_EQ(h_execute_query(&conn, "SELECT 1", nullptr, 0), H_OK);
  EXPECT_EQ(g_query_calls, 2u);
  EXPECT_EQ(g_reconnect_calls, 4u);
  ASSERT_EQ(g_sleep_calls, 3u);

  const long expected[] = {1000, 2000, 4000};
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(g_sleep_delays[i], expected[i]) << "backoff delay " << i;
  }
}

TEST(DbReconnect, RetriesExhausted)
{
  ResetMocks();
  g_query_results[0] = H_ERROR_CONNECTION;  // always fails
  g_query_results_len = 1;
  g_reconnect_results[0] = 1;               // always fails
  g_reconnect_results_len = 1;

  struct _h_connection conn;
  MakeConnection(&conn);

  EXPECT_EQ(h_execute_query(&conn, "SELECT 1", nullptr, 0), H_ERROR_CONNECTION);
  EXPECT_EQ(g_query_calls, 1u);           // no successful retry
  EXPECT_EQ(g_reconnect_calls, 6u);       // 1 immediate + 5 backoff (NOT 6 backoff)
  ASSERT_EQ(g_sleep_calls, 5u);           // exactly maxAttempts sleeps

  // Exponential sequence: 1000 -> 2000 -> 4000 -> 8000 -> 10000 (saturated).
  const long expected[] = {1000, 2000, 4000, 8000, 10000};
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(g_sleep_delays[i], expected[i]) << "backoff delay " << i;
  }
}
