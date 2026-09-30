/**
 * @file mock_driver.h
 * @brief A recording test double for the logger's private driver seam.
 *
 * The public interface offers no way to supply a driver — that is the point of
 * it — so a test that needs to observe what the core did with a record reaches
 * it through the module's injection hook.  This header is the C face of that
 * double.
 *
 * ## Why this header names no private type
 *
 * The seam's own types live in @c src/logger/logger_type_priv.h, which is
 * written in C17 and carries raw @c _Atomic members.  The gtest suite is C++,
 * where those do not parse.  Rather than bridge atomics into every test
 * translation unit, the driver table is kept behind the functions below: only
 * @c mock_driver.c includes the private headers, and every test sees public
 * types and nothing else.
 *
 * That also means a test cannot accidentally reach past the public contract to
 * assert on internals — if a test needs to know something, it has to be
 * expressible through the public interface or through this file.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_TESTS_LOGGER_MOCK_DRIVER_H
#define UFLIB_TESTS_LOGGER_MOCK_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include <uflib/logger/logger.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Records retained verbatim.  Writes beyond this are counted but not stored. */
#define MOCK_DRIVER_MAX_RECORDS 8192

/*! Room for a captured message. */
#define MOCK_DRIVER_MESSAGE_CAP 2048

/*! Room for a captured source file. */
#define MOCK_DRIVER_FILE_CAP 256

/*! Room for a captured source function. */
#define MOCK_DRIVER_FUNCTION_CAP 128

/*!
 * One record as the backend received it.
 *
 * Deliberately a copy, not a pointer into anything the core owns: the core
 * frees its formatted buffer as soon as @c write returns, so a double that
 * retained the pointer would be reading freed memory and would pass only
 * because nothing had overwritten it yet.
 */
typedef struct {
  UfLoggerLevel level;
  char          message[MOCK_DRIVER_MESSAGE_CAP];
  char          file[MOCK_DRIVER_FILE_CAP];
  char          function[MOCK_DRIVER_FUNCTION_CAP];
  int           line;
  uint64_t      sequence;
} MockDriverRecord;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/*!
 * Forget every counter and record.
 *
 * Call between test cases.  The double holds process-wide state, so a case that
 * does not reset cannot tell its own records from the previous case's.
 */
void MockDriverReset(void);

/*!
 * Set what the double claims it can honour, before a logger is created.
 *
 * The default is everything the real backend declares.  Lowering it is how the
 * capability-refusal path is reached without needing a second backend.
 */
void MockDriverSetCapabilities(uint32_t capabilities);

/*! The capability mask currently declared. */
uint32_t MockDriverCapabilities(void);

/*!
 * Make the next @c open fail with @p status.
 *
 * For the create-failure path: a caller must be able to learn that bringing a
 * backend up failed without the logger being left half-built.
 */
void MockDriverFailNextOpen(UfLoggerStatus status);

/*!
 * Create a logger bound to this double.
 *
 * The whole reason this header exists: it wraps the module's injection hook so
 * a C++ test never has to name the driver table.
 */
UfLoggerStatus MockDriverCreateLogger(const UfLoggerConfig *config_ptr, UfLogger **out_logger_ptr);

/* ── Observation ────────────────────────────────────────────────────────── */

int MockDriverOpenCount(void);
int MockDriverCloseCount(void);
int MockDriverWriteCount(void);
int MockDriverLevelSetCount(void);
int MockDriverFlushCount(void);
int MockDriverArchiveCount(void);
int MockDriverReloadCount(void);
int MockDriverContextSetCount(void);
int MockDriverContextRemoveCount(void);
int MockDriverContextClearCount(void);

/*! Records received at exactly @p level.  Exact, unlike the stored array. */
int MockDriverWriteCountForLevel(UfLoggerLevel level);

/*! Records retained; may be fewer than @ref MockDriverWriteCount. */
int MockDriverRecordCount(void);

/*! Record @p index, or NULL if it was not retained. */
const MockDriverRecord *MockDriverGetRecord(int index);

/*! The record most recently received, or NULL if none. */
const MockDriverRecord *MockDriverLastRecord(void);

/*! The level most recently passed to @c set_level. */
UfLoggerLevel MockDriverLastLevelSet(void);

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_TESTS_LOGGER_MOCK_DRIVER_H */
