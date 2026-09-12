/**
 * @file db_reconnect_driver.c
 * @brief Standalone probe for the DB connection-loss retry loop (Toxiproxy Layer 3).
 *
 * This is the client half of the Toxiproxy integration harness (see
 * dev/technical_designs/UFLIB_TESTING_MARIADB.md §7).  It links uflib + the real
 * libmariadb connector, opens a connection through a Toxiproxy front, and drives
 * h_execute_query the way a real consumer would — so the fault injected by the
 * orchestration script (toxiproxy_reconnect.sh) exercises the *production*
 * retry/backoff path, byte for byte.
 *
 * Design:
 *   - After connecting, it replaces the connection's on_sleep provider with a
 *     recording one that (a) prints every backoff delay and (b) still sleeps for
 *     it, so the retry loop's wall-clock timing is real.
 *   - It prints a READY line (flushed) immediately before issuing a blocking
 *     query, so the script knows the exact window in which to inject a fault.
 *   - It prints a RESULT line with the final return code and the recorded
 *     backoff sequence, then asserts that sequence against --expect.
 *
 * The driver issues read-only `SELECT` statements only.
 *
 * Usage:
 *   db_reconnect_driver --host 127.0.0.1 --port 33062 --user root \
 *       --passwd scratch --db scratch --sleep 5 --expect exhaustion
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#include <mariadb/mysql.h>
#include <uflib/db/db_sql.h>

#define MAX_BACKOFFS  32

/* Recorded backoff delays (milliseconds) and how many were observed. */
static long    s_backoff_delays[MAX_BACKOFFS];
static size_t  s_backoff_count = 0;

static long long
now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000);
}

/**
 * Recording on_sleep provider.  Prints and records the delay, then actually
 * sleeps for it (mirroring the production sDbBackoffSleepMs nanosleep loop).
 */
static int
sRecordSleep(long delay_ms)
{
  struct timespec ts = { delay_ms / 1000L, (delay_ms % 1000L) * 1000000L };

  if (s_backoff_count < MAX_BACKOFFS) {
    s_backoff_delays[s_backoff_count++] = delay_ms;
  }
  printf("[%lld] BACKOFF %ld\n", now_ms(), delay_ms);
  fflush(stdout);

  while (nanosleep(&ts, &ts) != 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return 0;
}

static void
usage(const char *prog)
{
  fprintf(stderr,
          "usage: %s --host H --port P --user U --passwd W --db D "
          "--sleep N [--query SQL] "
          "[--expect none|exhaustion|recovery|query_error|connect_down]\n", prog);
}

int
main(int argc, char **argv)
{
  const char *host   = "127.0.0.1";
  const char *user   = "root";
  const char *passwd = "scratch";
  const char *db     = "scratch";
  unsigned int port  = 33062;
  long sleep_secs    = 5;
  const char *expect = "none";
  const char *query_override = NULL;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)    host   = argv[++i];
    else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) user = argv[++i];
    else if (strcmp(argv[i], "--passwd") == 0 && i + 1 < argc) passwd = argv[++i];
    else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db = argv[++i];
    else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = (unsigned int)strtoul(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--sleep") == 0 && i + 1 < argc) sleep_secs = strtol(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) expect = argv[++i];
    else if (strcmp(argv[i], "--query") == 0 && i + 1 < argc) query_override = argv[++i];
    else {
      usage(argv[0]);
      return 2;
    }
  }

  if (sleep_secs <= 0) {
    fprintf(stderr, "--sleep must be positive\n");
    return 2;
  }

  if (mysql_library_init(0, NULL, NULL) != 0) {
    fprintf(stderr, "mysql_library_init failed\n");
    return 2;
  }

  struct _h_connection *conn =
      h_connect_mariadb(host, user, passwd, db, port, NULL);
  if (conn == NULL) {
    if (strcmp(expect, "connect_down") == 0) {
      printf("[%lld] CONNECT_FAIL (expected)\n", now_ms());
      fflush(stdout);
      mysql_library_end();
      return 0;
    }
    fprintf(stderr, "h_connect_mariadb(%s:%u) failed\n", host, port);
    mysql_library_end();
    return 2;
  }
  if (strcmp(expect, "connect_down") == 0) {
    fprintf(stderr, "connect_down: expected connect failure, but connected\n");
    h_close_mariadb(conn);
    mysql_library_end();
    return 1;
  }

  /* Record (and sleep) every backoff the retry loop asks for. */
  conn->backoff_descriptor.on_sleep = sRecordSleep;

  char query[256];
  if (query_override != NULL) {
    snprintf(query, sizeof(query), "%s", query_override);
  } else {
    snprintf(query, sizeof(query), "SELECT SLEEP(%ld)", sleep_secs);
  }

  printf("[%lld] READY\n", now_ms());
  fflush(stdout);

  int rc = h_execute_query(conn, query, NULL, 0);

  printf("[%lld] RESULT rc=%d backoffs=%zu\n", now_ms(), rc, s_backoff_count);
  fflush(stdout);

  int verdict = 0;

  if (strcmp(expect, "exhaustion") == 0) {
    /* Production sequence: 1000 -> 2000 -> 4000 -> 8000 -> 10000 (saturated). */
    const long expected[] = { 1000, 2000, 4000, 8000, 10000 };
    const size_t n = sizeof(expected) / sizeof(expected[0]);

    if (rc != H_ERROR_CONNECTION) {
      fprintf(stderr, "exhaustion: expected H_ERROR_CONNECTION (%d), got %d\n",
              H_ERROR_CONNECTION, rc);
      verdict = 1;
    }
    if (s_backoff_count != n) {
      fprintf(stderr, "exhaustion: expected %zu backoffs, got %zu\n", n, s_backoff_count);
      verdict = 1;
    }
    for (size_t i = 0; i < n && i < s_backoff_count; ++i) {
      if (s_backoff_delays[i] != expected[i]) {
        fprintf(stderr, "exhaustion: backoff[%zu] expected %ld, got %ld\n",
                i, expected[i], s_backoff_delays[i]);
        verdict = 1;
      }
    }
  } else if (strcmp(expect, "recovery") == 0) {
    /* Recovered: H_OK, at least one backoff, delays are a prefix of the
     * deterministic exponential sequence. */
    const long expected[] = { 1000, 2000, 4000, 8000, 10000 };

    if (rc != H_OK) {
      fprintf(stderr, "recovery: expected H_OK (%d), got %d\n", H_OK, rc);
      verdict = 1;
    }
    if (s_backoff_count == 0) {
      fprintf(stderr, "recovery: expected at least one backoff, got none\n");
      verdict = 1;
    }
    for (size_t i = 0; i < s_backoff_count; ++i) {
      if (i < sizeof(expected) / sizeof(expected[0]) &&
          s_backoff_delays[i] != expected[i]) {
        fprintf(stderr, "recovery: backoff[%zu] expected %ld, got %ld\n",
                i, expected[i], s_backoff_delays[i]);
        verdict = 1;
      }
    }
  } else if (strcmp(expect, "query_error") == 0) {
    /* A non-connection query error must NOT trigger the retry loop. */
    if (rc != H_ERROR_QUERY) {
      fprintf(stderr, "query_error: expected H_ERROR_QUERY (%d), got %d\n",
              H_ERROR_QUERY, rc);
      verdict = 1;
    }
    if (s_backoff_count != 0) {
      fprintf(stderr, "query_error: expected 0 backoffs, got %zu\n", s_backoff_count);
      verdict = 1;
    }
  }

  h_close_mariadb(conn);
  mysql_library_end();

  return verdict;
}
