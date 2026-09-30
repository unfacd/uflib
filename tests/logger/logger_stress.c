/**
 * @file logger_stress.c
 * @brief Standalone multi-threaded stress for the logger.
 *
 * Not registered with CTest: it takes arguments and runs for a caller-chosen
 * duration, so it is a tool rather than a gate.  It exits non-zero on any
 * discrepancy, which is what makes it usable in a longer soak.
 *
 * ## Running it under a sanitizer preset
 *
 * The dev presets build with LeakSanitizer, and `--driver zlog` will therefore
 * end by reporting ISS-001 — zlog's own configuration leak, 121,184 bytes for
 * the single logger this tool holds.  That is expected and is not this tool
 * failing.  To see only the accounting:
 *
 *   LSAN_OPTIONS=suppressions=tests/logger/lsan_suppressions.txt \
 *       ./build/<preset>/tests/logger/logger_stress --driver zlog
 *
 * ## What it is trying to catch
 *
 * The invariant is exact accounting.  Every worker counts what it asked to be
 * delivered, and the destination counts what it received; under a floor that
 * suppresses nothing the two must agree exactly.  A lock-free or racy logging
 * path fails this by losing records, duplicating them, or corrupting the
 * formatted text — none of which a single-threaded test can observe.
 *
 * Two backends, because they exercise different machinery:
 *
 *   --driver mock   the recording double.  Exact per-level accounting, plus an
 *                   optional churn thread creating and destroying loggers
 *                   throughout, which is what exercises the core's lifecycle
 *                   under concurrency.
 *   --driver zlog   the real backend, writing to a file.  Accounting is by
 *                   counting lines, and it is deliberately emit-only: creating
 *                   and destroying loggers in a loop would install the
 *                   process-global configuration repeatedly, which is a known
 *                   upstream leak (see the design document's issues register)
 *                   and would measure that rather than this.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/logger/logger.h>

#include "mock_driver.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

/* ── Options ────────────────────────────────────────────────────────────── */

#define STRESS_DEFAULT_THREADS   8
#define STRESS_DEFAULT_DURATION  5
#define STRESS_MAX_THREADS       64
#define STRESS_OUTPUT_PATH       "/tmp/ufsrv_logger_stress.log"

typedef struct {
  int  threads;
  int  duration_seconds;
  bool is_zlog_driver;
  bool is_churn_enabled;
} Options;

/* ── Shared state ───────────────────────────────────────────────────────── */

static _Atomic bool s_stop;
static _Atomic long s_emitted;
static _Atomic long s_errors;
static _Atomic long s_per_level[UF_LOGGER_LEVEL_FATAL + 1u];

/* The churn thread's records reach the same destination as everyone else's, so
   they are part of what must be accounted for.  They are counted separately
   only because they are emitted at a fixed rank by a thread that does not take
   part in the per-rank loop below. */
static _Atomic long s_churn_emitted;

/* ── Workers ────────────────────────────────────────────────────────────── */

typedef struct {
  UfLogger *log_ptr;
  int       index;
} EmitterArg;

/*!
 * The emitter's message is deliberately varied in length and content: a fixed
 * short string would exercise one path through the formatting buffer and one
 * only, and it is the transitions between the stack buffer and the allocation
 * that a race is most likely to corrupt.
 */
static void *
sEmitter(void *arg_ptr)
{
  EmitterArg *arg = arg_ptr;
  unsigned    state = 0x9E3779B9u ^ (unsigned)arg->index;
  char        message[256];

  while (!atomic_load_explicit(&s_stop, memory_order_relaxed)) {
    UfLoggerLevel level;
    int           length;

    state = state * 1103515245u + 12345u;
    level = (UfLoggerLevel)(UF_LOGGER_LEVEL_TRACE + (state % 8u));

    state = state * 1103515245u + 12345u;
    length = (int)(state % 200u);

    memset(message, 'x', sizeof message);
    message[length] = '\0';

    if (UfLoggerLog(arg->log_ptr, level, __FILE__, __LINE__, __func__, "%s|%d", message,
                    arg->index) != UF_LOGGER_STATUS_OK) {
      atomic_fetch_add_explicit(&s_errors, 1, memory_order_relaxed);
      continue;
    }

    atomic_fetch_add_explicit(&s_emitted, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_per_level[level], 1, memory_order_relaxed);
  }

  return NULL;
}

/*!
 * Creates and destroys its own loggers throughout the run.
 *
 * This is what a server doing lazy per-subsystem logging does, and it is the
 * path where a lifecycle race would show up as a crash or a corrupted shared
 * configuration rather than as a wrong count.
 */
static void *
sChurner(void *arg_ptr)
{
  const Options *options = arg_ptr;
  UfLoggerConfig base;
  unsigned       state = 0x1234567u;

  memset(&base, 0, sizeof base);
  base.minimum_level = UF_LOGGER_LEVEL_TRACE;
  base.destination   = UF_LOGGER_DESTINATION_SYSLOG;

  while (!atomic_load_explicit(&s_stop, memory_order_relaxed)) {
    UfLoggerConfig config = base;
    char           name[64];
    UfLogger      *log_ptr = NULL;

    state = state * 1103515245u + 12345u;
    snprintf(name, sizeof name, "churn_%u", state % 1000u);
    config.category = name;

    if (MockDriverCreateLogger(&config, &log_ptr) != UF_LOGGER_STATUS_OK) {
      atomic_fetch_add_explicit(&s_errors, 1, memory_order_relaxed);
      continue;
    }

    if (UF_LOGGER_INFO(log_ptr, "churn") != UF_LOGGER_STATUS_OK) {
      atomic_fetch_add_explicit(&s_errors, 1, memory_order_relaxed);
    } else {
      atomic_fetch_add_explicit(&s_churn_emitted, 1, memory_order_relaxed);
    }

    UfLoggerDestroy(log_ptr);
    (void)options;
  }

  return NULL;
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

static double
sNowSeconds(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static long
sCountLines(const char *path_ptr)
{
  FILE *file_ptr = fopen(path_ptr, "rb");
  long  lines = 0;
  int   c;

  if (!file_ptr) return -1;

  while ((c = fgetc(file_ptr)) != EOF) {
    if (c == '\n') lines++;
  }
  fclose(file_ptr);
  return lines;
}

/*!
 * Count the terminated lines of @p base_path and every archive beside it.
 *
 * The file destination rotates at a configured size, so counting only the live
 * file would miss everything already rolled aside and report a shortfall that
 * is an artifact of the accounting rather than a lost record.  Archives sit
 * next to the file and share its name, which is how they are found here.
 */
static long
sCountLinesInFamily(const char *base_path)
{
  char        directory[1024];
  const char *base_name;
  const char *slash_ptr = strrchr(base_path, '/');
  DIR        *dir_ptr;
  struct dirent *entry_ptr;
  long        total = 0;
  size_t      base_len;

  if (slash_ptr) {
    size_t length = (size_t)(slash_ptr - base_path);

    if (length >= sizeof directory) return -1;
    memcpy(directory, base_path, length);
    directory[length] = '\0';
    base_name = slash_ptr + 1;
  } else {
    snprintf(directory, sizeof directory, ".");
    base_name = base_path;
  }
  base_len = strlen(base_name);

  dir_ptr = opendir(directory);
  if (!dir_ptr) return -1;

  while ((entry_ptr = readdir(dir_ptr)) != NULL) {
    char  candidate[2048];
    long  lines;

    if (strncmp(entry_ptr->d_name, base_name, base_len) != 0) continue;
    if (snprintf(candidate, sizeof candidate, "%s/%s", directory, entry_ptr->d_name) < 0) continue;

    lines = sCountLines(candidate);
    if (lines > 0) total += lines;
  }
  closedir(dir_ptr);
  return total;
}

/*! Remove @p base_path and every archive beside it. */
static void
sRemoveFamily(const char *base_path)
{
  char        directory[1024];
  const char *base_name;
  const char *slash_ptr = strrchr(base_path, '/');
  DIR        *dir_ptr;
  struct dirent *entry_ptr;
  size_t      base_len;

  if (slash_ptr) {
    size_t length = (size_t)(slash_ptr - base_path);

    if (length >= sizeof directory) return;
    memcpy(directory, base_path, length);
    directory[length] = '\0';
    base_name = slash_ptr + 1;
  } else {
    snprintf(directory, sizeof directory, ".");
    base_name = base_path;
  }
  base_len = strlen(base_name);

  dir_ptr = opendir(directory);
  if (!dir_ptr) return;

  while ((entry_ptr = readdir(dir_ptr)) != NULL) {
    char candidate[2048];

    if (strncmp(entry_ptr->d_name, base_name, base_len) != 0) continue;
    if (snprintf(candidate, sizeof candidate, "%s/%s", directory, entry_ptr->d_name) < 0) continue;
    remove(candidate);
  }
  closedir(dir_ptr);
}

static void
sUsage(const char *program_ptr)
{
  fprintf(stderr,
          "usage: %s [--threads N] [--duration S] [--driver mock|zlog] [--churn]\n"
          "\n"
          "  --threads N     emitter threads (default %d, max %d)\n"
          "  --duration S    seconds to run (default %d)\n"
          "  --driver NAME   'mock' for exact accounting (default), 'zlog' for the real backend\n"
          "  --churn         also run a thread creating and destroying loggers (mock only)\n",
          program_ptr, STRESS_DEFAULT_THREADS, STRESS_MAX_THREADS, STRESS_DEFAULT_DURATION);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
  Options        options = { STRESS_DEFAULT_THREADS, STRESS_DEFAULT_DURATION, false, false };
  pthread_t      emitters[STRESS_MAX_THREADS];
  EmitterArg     emitter_args[STRESS_MAX_THREADS];
  pthread_t      churner;
  bool           have_churner = false;
  UfLogger      *log_ptr = NULL;
  UfLoggerConfig config;
  double         started;
  double         elapsed;
  long           emitted;
  long           churn_emitted;
  long           received;
  long           errors;
  int            failures = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      options.threads = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
      options.duration_seconds = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--driver") == 0 && i + 1 < argc) {
      options.is_zlog_driver = (strcmp(argv[++i], "zlog") == 0);
    } else if (strcmp(argv[i], "--churn") == 0) {
      options.is_churn_enabled = true;
    } else {
      sUsage(argv[0]);
      return 2;
    }
  }

  if (options.threads < 1 || options.threads > STRESS_MAX_THREADS || options.duration_seconds < 1) {
    sUsage(argv[0]);
    return 2;
  }

  if (options.is_churn_enabled && options.is_zlog_driver) {
    fprintf(stderr, "stress: --churn is not available with --driver zlog; see the file header\n");
    return 2;
  }

  /* The floor is TRACE so that nothing is suppressed and the accounting is
     exact.  A stress run that filtered records would be comparing two numbers
     that were never meant to be equal. */
  memset(&config, 0, sizeof config);
  config.category      = "stress";
  config.minimum_level = UF_LOGGER_LEVEL_TRACE;

  if (options.is_zlog_driver) {
    sRemoveFamily(STRESS_OUTPUT_PATH);
    config.destination    = UF_LOGGER_DESTINATION_FILE;
    config.file_path      = STRESS_OUTPUT_PATH;

    /* Every line ends with a newline, so the file can be counted.  Without the
       terminator the whole file would be one line and the count meaningless. */
    config.format_pattern = "%V|%m%n";
  } else {
    config.destination    = UF_LOGGER_DESTINATION_SYSLOG;
    config.format_pattern = "%V|%m%n";
  }

  printf("==> logger stress: threads=%d duration=%ds driver=%s churn=%s\n",
         options.threads, options.duration_seconds,
         options.is_zlog_driver ? "zlog" : "mock", options.is_churn_enabled ? "yes" : "no");

  if (options.is_zlog_driver) {
    if (UfLoggerCreate(&config, &log_ptr) != UF_LOGGER_STATUS_OK || !log_ptr) {
      fprintf(stderr, "stress: could not create the logger\n");
      return 1;
    }
  } else {
    MockDriverReset();
    if (MockDriverCreateLogger(&config, &log_ptr) != UF_LOGGER_STATUS_OK || !log_ptr) {
      fprintf(stderr, "stress: could not create the logger\n");
      return 1;
    }
  }

  atomic_store_explicit(&s_stop, false, memory_order_relaxed);
  started = sNowSeconds();

  for (int i = 0; i < options.threads; i++) {
    emitter_args[i].log_ptr = log_ptr;
    emitter_args[i].index   = i;
    if (pthread_create(&emitters[i], NULL, sEmitter, &emitter_args[i]) != 0) {
      fprintf(stderr, "stress: could not start emitter %d\n", i);
      atomic_store_explicit(&s_stop, true, memory_order_relaxed);
      return 1;
    }
  }

  if (options.is_churn_enabled) {
    if (pthread_create(&churner, NULL, sChurner, &options) == 0) {
      have_churner = true;
    } else {
      fprintf(stderr, "stress: could not start the churn thread\n");
      failures++;
    }
  }

  while ((elapsed = sNowSeconds() - started) < (double)options.duration_seconds) {
    struct timespec pause = { 0, 100000000L };   /* 100 ms */
    nanosleep(&pause, NULL);
  }

  atomic_store_explicit(&s_stop, true, memory_order_relaxed);

  for (int i = 0; i < options.threads; i++) pthread_join(emitters[i], NULL);
  if (have_churner) pthread_join(churner, NULL);

  elapsed       = sNowSeconds() - started;
  emitted       = atomic_load_explicit(&s_emitted, memory_order_relaxed);
  churn_emitted = atomic_load_explicit(&s_churn_emitted, memory_order_relaxed);
  errors        = atomic_load_explicit(&s_errors, memory_order_relaxed);

  /* The churn thread's records reach the same destination, so they belong in
     the expected total.  Leaving them out is how this harness first reported a
     discrepancy that was its own rather than the module's. */
  if (options.is_churn_enabled) emitted += churn_emitted;

  UfLoggerDestroy(log_ptr);

  /* ── Accounting ───────────────────────────────────────────────────────── */

  if (options.is_zlog_driver) {
    /* Every record ends in a newline and the logger has already been destroyed,
       which flushes and closes the file.  A shortfall is therefore a real
       discrepancy and is reported as one — absorbing "one line fewer" here
       would be exactly the kind of leniency that hides a lost record. */
    received = sCountLinesInFamily(STRESS_OUTPUT_PATH);
  } else {
    received = MockDriverWriteCount();
  }

  printf("    emitted   : %ld\n", emitted);
  printf("    received  : %ld\n", received);
  printf("    errors    : %ld\n", errors);
  printf("    elapsed   : %.2fs (%.0f records/s)\n", elapsed,
         (elapsed > 0.0) ? (double)emitted / elapsed : 0.0);

  if (emitted == 0) {
    fprintf(stderr, "  FAIL: no records were emitted at all\n");
    failures++;
  }
  if (errors != 0) {
    fprintf(stderr, "  FAIL: %ld emit call(s) reported an error\n", errors);
    failures++;
  }
  if (received != emitted) {
    fprintf(stderr, "  FAIL: %ld records accounted for, %ld emitted (lost or duplicated)\n",
            received, emitted);
    failures++;
  }

  if (!options.is_zlog_driver) {
    long per_level_total = 0;

    for (unsigned level = UF_LOGGER_LEVEL_TRACE; level <= UF_LOGGER_LEVEL_FATAL; level++) {
      long count = atomic_load_explicit(&s_per_level[level], memory_order_relaxed);

      /* The churn thread emits at INFO, so its records belong to that rank. */
      if (options.is_churn_enabled && level == UF_LOGGER_LEVEL_INFO) count += churn_emitted;

      per_level_total += count;

      if (count != MockDriverWriteCountForLevel((UfLoggerLevel)level)) {
        fprintf(stderr, "  FAIL: rank %u delivered %d, emitted %ld\n", level,
                MockDriverWriteCountForLevel((UfLoggerLevel)level), count);
        failures++;
      }
    }

    if (per_level_total != emitted) {
      fprintf(stderr, "  FAIL: per-rank totals sum to %ld, not %ld\n", per_level_total, emitted);
      failures++;
    }
  }

  if (failures != 0) {
    fprintf(stderr, "logger stress: FAIL (%d)\n", failures);
    return 1;
  }

  puts("logger stress: PASS");
  return 0;
}
