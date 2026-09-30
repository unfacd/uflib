/**
 * @file mock_driver.c
 * @brief The recording test double's implementation.
 *
 * The only file in the test suite that includes the logger's private headers,
 * which is what keeps raw C17 atomics out of the C++ test translation units.
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

#include "mock_driver.h"

#include "logger_priv.h"
#include "logger_type_priv.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

/* ── State ──────────────────────────────────────────────────────────────── */

typedef struct {
  _Atomic int opens;
  _Atomic int closes;
  _Atomic int writes;
  _Atomic int level_sets;
  _Atomic int flushes;
  _Atomic int archives;
  _Atomic int reloads;
  _Atomic int context_sets;
  _Atomic int context_removes;
  _Atomic int context_clears;
  _Atomic int per_level[UF_LOGGER_LEVEL_FATAL + 1u];
  _Atomic int stored;

  pthread_mutex_t  records_lock;
  MockDriverRecord records[MOCK_DRIVER_MAX_RECORDS];
  _Atomic uint64_t sequence;
} MockState;

/* Statically initialised rather than zeroed: a mutex that was never given an
   initialiser is not one this code may lock, even though a zeroed one happens
   to behave correctly on glibc today. */
static MockState s_state = { .records_lock = PTHREAD_MUTEX_INITIALIZER };

/*! Capabilities the double claims.  Set before a logger exists. */
static uint32_t s_capabilities = UF_LOGGER_CAP_RUNTIME_LEVEL | UF_LOGGER_CAP_RELOAD |
                                 UF_LOGGER_CAP_FLUSH | UF_LOGGER_CAP_MANUAL_ARCHIVE |
                                 UF_LOGGER_CAP_FILE_ROTATION | UF_LOGGER_CAP_CONTEXT;

/*! Distinct capability masks the double will hand out, one immutable table each. */
#define MOCK_DRIVER_MAX_CAPABILITY_VARIANTS 8u

static LoggerDriverTranslationTable s_tables[MOCK_DRIVER_MAX_CAPABILITY_VARIANTS];
static uint32_t                    s_table_masks[MOCK_DRIVER_MAX_CAPABILITY_VARIANTS];
static unsigned                    s_table_count;
static pthread_mutex_t             s_table_lock = PTHREAD_MUTEX_INITIALIZER;

/*! Status the next @c open should fail with, or OK for none pending. */
static UfLoggerStatus s_fail_next_open = UF_LOGGER_STATUS_OK;

static UfLoggerLevel s_last_level_set = UF_LOGGER_LEVEL_INFO;

/* ── Slot implementations ───────────────────────────────────────────────── */

static UfLoggerStatus
mOpen(void **out_state_ptr, const LoggerCompiledConfig *config_ptr)
{
  if (!out_state_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;

  /* The compiled model must arrive populated: a backend that received an empty
     one would be entitled to produce nothing, and a test would then pass for
     the wrong reason. */
  if (!config_ptr || config_ptr->rule_count == 0u || !config_ptr->category ||
      config_ptr->level_map_count == 0u) {
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }

  if (s_fail_next_open != UF_LOGGER_STATUS_OK) {
    UfLoggerStatus status = s_fail_next_open;
    s_fail_next_open = UF_LOGGER_STATUS_OK;
    return status;
  }

  atomic_fetch_add_explicit(&s_state.opens, 1, memory_order_relaxed);
  *out_state_ptr = &s_state;
  return UF_LOGGER_STATUS_OK;
}

static void
mClose(void *state_ptr)
{
  if (!state_ptr) return;
  atomic_fetch_add_explicit(&s_state.closes, 1, memory_order_relaxed);
}

static UfLoggerStatus
mWrite(void *state_ptr, UfLoggerLevel level, const char *message_ptr,
       const char *file_ptr, int line, const char *function_ptr)
{
  MockDriverRecord *record_ptr;
  int               slot;

  if (!state_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  if ((unsigned)level > UF_LOGGER_LEVEL_FATAL) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;

  atomic_fetch_add_explicit(&s_state.writes, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&s_state.per_level[level], 1, memory_order_relaxed);

  slot = atomic_fetch_add_explicit(&s_state.stored, 1, memory_order_relaxed);
  if (slot >= MOCK_DRIVER_MAX_RECORDS) return UF_LOGGER_STATUS_OK;

  record_ptr = &s_state.records[slot];

  /* Copied, not retained: the core frees its formatted buffer the moment this
     returns, so holding the pointer would be reading freed memory. */
  record_ptr->level = level;
  record_ptr->line  = line;
  record_ptr->sequence = atomic_fetch_add_explicit(&s_state.sequence, 1u, memory_order_relaxed);
  snprintf(record_ptr->message, sizeof(record_ptr->message), "%s", message_ptr ? message_ptr : "");
  snprintf(record_ptr->file, sizeof(record_ptr->file), "%s", file_ptr ? file_ptr : "");
  snprintf(record_ptr->function, sizeof(record_ptr->function), "%s",
           function_ptr ? function_ptr : "");

  return UF_LOGGER_STATUS_OK;
}

static UfLoggerStatus
mSetLevel(void *state_ptr, UfLoggerLevel level)
{
  if (!state_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;

  atomic_fetch_add_explicit(&s_state.level_sets, 1, memory_order_relaxed);
  s_last_level_set = level;
  return UF_LOGGER_STATUS_OK;
}

static UfLoggerStatus
mFlush(void *state_ptr)
{
  if (!state_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  atomic_fetch_add_explicit(&s_state.flushes, 1, memory_order_relaxed);
  return UF_LOGGER_STATUS_OK;
}

static UfLoggerStatus
mArchive(void *state_ptr)
{
  if (!state_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  atomic_fetch_add_explicit(&s_state.archives, 1, memory_order_relaxed);
  return UF_LOGGER_STATUS_OK;
}

static UfLoggerStatus
mReload(void *state_ptr, const LoggerCompiledConfig *config_ptr)
{
  if (!state_ptr || !config_ptr || config_ptr->rule_count == 0u) {
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }
  atomic_fetch_add_explicit(&s_state.reloads, 1, memory_order_relaxed);
  return UF_LOGGER_STATUS_OK;
}

static UfLoggerStatus
mSetContext(void *state_ptr, const char *key_ptr, const char *value_ptr)
{
  if (!state_ptr || !key_ptr || !value_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  atomic_fetch_add_explicit(&s_state.context_sets, 1, memory_order_relaxed);
  return UF_LOGGER_STATUS_OK;
}

static UfLoggerStatus
mRemoveContext(void *state_ptr, const char *key_ptr)
{
  if (!state_ptr || !key_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  atomic_fetch_add_explicit(&s_state.context_removes, 1, memory_order_relaxed);
  return UF_LOGGER_STATUS_OK;
}

static UfLoggerStatus
mClearContext(void *state_ptr)
{
  if (!state_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  atomic_fetch_add_explicit(&s_state.context_clears, 1, memory_order_relaxed);
  return UF_LOGGER_STATUS_OK;
}

/* ── The table ──────────────────────────────────────────────────────────── */

static const LoggerDriverTranslationTable sTable = {
  .open           = mOpen,
  .close          = mClose,
  .write          = mWrite,
  .set_level      = mSetLevel,
  .flush          = mFlush,
  .archive        = mArchive,
  .reload         = mReload,
  .set_context    = mSetContext,
  .remove_context = mRemoveContext,
  .clear_context  = mClearContext,
  .capabilities   = 0u   /* read from s_capabilities by the wrapper below */
};

/* ── The C face ─────────────────────────────────────────────────────────── */

/*!
 * The table to hand out for a given capability mask.
 *
 * A table the module holds for a logger's lifetime must not be rewritten
 * afterwards.  Copying the shared table per call and stamping the mask onto the
 * copy does exactly that, and races with any concurrent read of a table already
 * handed out — including the function pointers the module dereferences.  So
 * each distinct mask gets its own slot, written once, under the lock, before
 * the slot is published.
 *
 * Returns NULL when more masks have been asked for than there are slots, which
 * is a test-scale limit rather than a real one.
 */
static const LoggerDriverTranslationTable *
sTableForCapabilities(uint32_t capabilities)
{
  const LoggerDriverTranslationTable *published_ptr;

  pthread_mutex_lock(&s_table_lock);

  for (unsigned i = 0u; i < s_table_count; i++) {
    if (s_table_masks[i] == capabilities) {
      published_ptr = &s_tables[i];
      pthread_mutex_unlock(&s_table_lock);
      return published_ptr;
    }
  }

  if (s_table_count >= MOCK_DRIVER_MAX_CAPABILITY_VARIANTS) {
    pthread_mutex_unlock(&s_table_lock);
    return NULL;
  }

  s_tables[s_table_count]              = sTable;
  s_tables[s_table_count].capabilities = capabilities;
  s_table_masks[s_table_count]         = capabilities;

  published_ptr = &s_tables[s_table_count];
  s_table_count++;

  pthread_mutex_unlock(&s_table_lock);
  return published_ptr;
}

UfLoggerStatus
MockDriverCreateLogger(const UfLoggerConfig *config_ptr, UfLogger **out_logger_ptr)
{
  const LoggerDriverTranslationTable *table_ptr = sTableForCapabilities(s_capabilities);

  if (!table_ptr) return UF_LOGGER_STATUS_ERR_NOMEM;

  return LoggerCreateWithDriver(config_ptr, table_ptr, out_logger_ptr);
}

void
MockDriverReset(void)
{
  pthread_mutex_lock(&s_state.records_lock);

  atomic_store_explicit(&s_state.opens, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.closes, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.writes, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.level_sets, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.flushes, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.archives, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.reloads, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.context_sets, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.context_removes, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.context_clears, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.stored, 0, memory_order_relaxed);
  atomic_store_explicit(&s_state.sequence, 0u, memory_order_relaxed);

  for (unsigned i = 0u; i <= UF_LOGGER_LEVEL_FATAL; i++) {
    atomic_store_explicit(&s_state.per_level[i], 0, memory_order_relaxed);
  }
  memset(s_state.records, 0, sizeof(s_state.records));

  s_capabilities   = UF_LOGGER_CAP_RUNTIME_LEVEL | UF_LOGGER_CAP_RELOAD |
                     UF_LOGGER_CAP_FLUSH | UF_LOGGER_CAP_MANUAL_ARCHIVE |
                     UF_LOGGER_CAP_FILE_ROTATION | UF_LOGGER_CAP_CONTEXT;
  s_fail_next_open = UF_LOGGER_STATUS_OK;
  s_last_level_set = UF_LOGGER_LEVEL_INFO;

  pthread_mutex_unlock(&s_state.records_lock);
}

void MockDriverSetCapabilities(uint32_t capabilities) { s_capabilities = capabilities; }
uint32_t MockDriverCapabilities(void) { return s_capabilities; }
void MockDriverFailNextOpen(UfLoggerStatus status) { s_fail_next_open = status; }

int MockDriverOpenCount(void) { return atomic_load_explicit(&s_state.opens, memory_order_relaxed); }
int MockDriverCloseCount(void) { return atomic_load_explicit(&s_state.closes, memory_order_relaxed); }
int MockDriverWriteCount(void) { return atomic_load_explicit(&s_state.writes, memory_order_relaxed); }
int MockDriverLevelSetCount(void) { return atomic_load_explicit(&s_state.level_sets, memory_order_relaxed); }
int MockDriverFlushCount(void) { return atomic_load_explicit(&s_state.flushes, memory_order_relaxed); }
int MockDriverArchiveCount(void) { return atomic_load_explicit(&s_state.archives, memory_order_relaxed); }
int MockDriverReloadCount(void) { return atomic_load_explicit(&s_state.reloads, memory_order_relaxed); }
int MockDriverContextSetCount(void) { return atomic_load_explicit(&s_state.context_sets, memory_order_relaxed); }
int MockDriverContextRemoveCount(void) { return atomic_load_explicit(&s_state.context_removes, memory_order_relaxed); }
int MockDriverContextClearCount(void) { return atomic_load_explicit(&s_state.context_clears, memory_order_relaxed); }

int
MockDriverWriteCountForLevel(UfLoggerLevel level)
{
  if ((unsigned)level > UF_LOGGER_LEVEL_FATAL) return 0;
  return atomic_load_explicit(&s_state.per_level[level], memory_order_relaxed);
}

int
MockDriverRecordCount(void)
{
  int stored = atomic_load_explicit(&s_state.stored, memory_order_relaxed);
  return (stored > MOCK_DRIVER_MAX_RECORDS) ? MOCK_DRIVER_MAX_RECORDS : stored;
}

const MockDriverRecord *
MockDriverGetRecord(int index)
{
  if (index < 0 || index >= MockDriverRecordCount()) return NULL;
  return &s_state.records[index];
}

const MockDriverRecord *
MockDriverLastRecord(void)
{
  int count = MockDriverRecordCount();
  return (count > 0) ? &s_state.records[count - 1] : NULL;
}

UfLoggerLevel MockDriverLastLevelSet(void) { return s_last_level_set; }
