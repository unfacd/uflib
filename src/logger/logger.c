/**
 * @file logger.c
 * @brief The logger core: configuration compilation, lifecycle, and the public
 *        interface's implementation.
 *
 * This file holds the whole of the module's public behaviour and none of its
 * backend.  It compiles what a caller asked for into the routing model a
 * backend consumes, then hands records to whichever backend the module bound.
 * It never names a backend, and nothing in it knows what one is.
 *
 * ## The two layers this file sits between
 *
 * Above it is @ref UfLoggerConfig — eight fields a caller is entitled to set.
 * Below it is @ref LoggerCompiledConfig — the routing model, including the
 * severity map, buffer sizing and rotation scheme that a caller is not.
 * @ref LoggerCompileConfig is the boundary, and it is deliberately a real
 * function rather than a set of inline resolutions, because a configuration
 * file will one day arrive at the same model by a different route.
 *
 * ## Ownership
 *
 * A logger owns three things: its compiled rule array, the driver state its
 * backend opened, and nothing else.  The category is held inline so that a
 * caller's string need not outlive its creator, and everything else the
 * compiled model points at is a literal or the caller's own static.
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

#include "logger_priv.h"
#include "logger_type_priv.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* ── The canonical severity map ─────────────────────────────────────────── */

/*!
 * The module's rank-to-backend mapping.
 *
 * A caller works in ranks; a backend works in its own integers and syslog
 * severities.  This is where the two meet, and it is fixed rather than
 * configurable on purpose: nothing a caller can express should let it declare
 * that its CRITICAL is the backend's DEBUG.
 *
 * The integers are ascending and gapped so that a backend with its own scale
 * can be accommodated by editing this table and nothing else.
 */
static const LoggerLevelMapEntry sLevelMap[] = {
  { UF_LOGGER_LEVEL_TRACE,    "TRACE",    10u,  LOG_DEBUG   },
  { UF_LOGGER_LEVEL_DEBUG,    "DEBUG",    20u,  LOG_DEBUG   },
  { UF_LOGGER_LEVEL_INFO,     "INFO",     40u,  LOG_INFO    },
  { UF_LOGGER_LEVEL_NOTICE,   "NOTICE",   60u,  LOG_NOTICE  },
  { UF_LOGGER_LEVEL_WARN,     "WARN",     80u,  LOG_WARNING },
  { UF_LOGGER_LEVEL_ERROR,    "ERROR",   100u,  LOG_ERR     },
  { UF_LOGGER_LEVEL_CRITICAL, "CRITICAL",110u,  LOG_CRIT    },
  { UF_LOGGER_LEVEL_FATAL,    "FATAL",   120u,  LOG_ALERT   }
};

#define UFLOGGER_LEVEL_MAP_COUNT (sizeof(sLevelMap) / sizeof(sLevelMap[0]))

/* ── The default configuration ──────────────────────────────────────────── */

/*!
 * What @ref UfLoggerProvideSaneDefaults hands back.
 *
 * A working configuration rather than an empty one: a caller that takes this
 * and changes nothing gets INFO and above in syslog, which is what the servers
 * this library serves do today.
 */
static const UfLoggerConfig sSaneDefaults = {
  .category        = CONFIG_DEFAULT_UFLOGGER_CATEGORY,
  .minimum_level   = CONFIG_DEFAULT_UFLOGGER_LEVEL,
  .destination     = UF_LOGGER_DESTINATION_SYSLOG,
  .file_path       = NULL,
  .max_file_bytes  = 0u,
  .max_archives    = 0u,
  .syslog_facility = CONFIG_DEFAULT_UFLOGGER_SYSLOG_FACILITY,
  .format_pattern  = CONFIG_DEFAULT_UFLOGGER_FORMAT
};

/* ── Small helpers ──────────────────────────────────────────────────────── */

static bool
sIsRealLevel(UfLoggerLevel level)
{
  return level >= UF_LOGGER_LEVEL_TRACE && level <= UF_LOGGER_LEVEL_FATAL;
}

/*!
 * The spelling of a rank.
 *
 * Read from the map rather than from a second switch here, so that the name a
 * description prints and the name a backend is told to configure are the same
 * string rather than two that have to be kept equal.
 */
static const char *
sLevelName(UfLoggerLevel level)
{
  for (size_t i = 0; i < UFLOGGER_LEVEL_MAP_COUNT; i++) {
    if (sLevelMap[i].level == level) return sLevelMap[i].name;
  }

  return "DEFAULT";
}

static const char *
sDestinationName(UfLoggerDestination destination)
{
  switch (destination) {
  case UF_LOGGER_DESTINATION_SYSLOG: return "syslog";
  case UF_LOGGER_DESTINATION_STDERR: return "stderr";
  case UF_LOGGER_DESTINATION_STDOUT: return "stdout";
  case UF_LOGGER_DESTINATION_FILE:   return "file";
  default:                           return "unknown";
  }
}

/*!
 * Map a caller's facility onto the platform's.
 *
 * @c LOG_USER for an unset field, matching @c openlog(3) as the servers in this
 * fleet already call it.
 */
static int
sFacilityToSyslog(UfLoggerSyslogFacility facility)
{
  switch (facility) {
  case UF_LOGGER_SYSLOG_FACILITY_DAEMON: return LOG_DAEMON;
  case UF_LOGGER_SYSLOG_FACILITY_LOCAL0: return LOG_LOCAL0;
  case UF_LOGGER_SYSLOG_FACILITY_LOCAL1: return LOG_LOCAL1;
  case UF_LOGGER_SYSLOG_FACILITY_LOCAL2: return LOG_LOCAL2;
  case UF_LOGGER_SYSLOG_FACILITY_LOCAL3: return LOG_LOCAL3;
  case UF_LOGGER_SYSLOG_FACILITY_LOCAL4: return LOG_LOCAL4;
  case UF_LOGGER_SYSLOG_FACILITY_LOCAL5: return LOG_LOCAL5;
  case UF_LOGGER_SYSLOG_FACILITY_LOCAL6: return LOG_LOCAL6;
  case UF_LOGGER_SYSLOG_FACILITY_LOCAL7: return LOG_LOCAL7;
  case UF_LOGGER_SYSLOG_FACILITY_USER:
  case UF_LOGGER_SYSLOG_FACILITY_DEFAULT:
  default:                               return LOG_USER;
  }
}

static LoggerRuleOutputKind
sDestinationToOutputKind(UfLoggerDestination destination)
{
  switch (destination) {
  case UF_LOGGER_DESTINATION_STDERR: return LOGGER_OUTPUT_STDERR;
  case UF_LOGGER_DESTINATION_STDOUT: return LOGGER_OUTPUT_STDOUT;
  case UF_LOGGER_DESTINATION_FILE:   return LOGGER_OUTPUT_FILE;
  case UF_LOGGER_DESTINATION_SYSLOG:
  default:                           return LOGGER_OUTPUT_SYSLOG;
  }
}

/* ── Configuration compiler ─────────────────────────────────────────────── */

UfLoggerStatus
LoggerCompileConfig(const UfLoggerConfig *config_ptr, LoggerCompiledConfig *out_config_ptr)
{
  LoggerRule   *rule_ptr;
  size_t        pattern_len;
  size_t        path_len;
  UfLoggerLevel level;

  if (!config_ptr || !out_config_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;

  memset(out_config_ptr, 0, sizeof(*out_config_ptr));

  level = (config_ptr->minimum_level == UF_LOGGER_LEVEL_DEFAULT)
              ? CONFIG_DEFAULT_UFLOGGER_LEVEL
              : config_ptr->minimum_level;
  if (!sIsRealLevel(level)) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;

  if (config_ptr->format_pattern) {
    pattern_len = strlen(config_ptr->format_pattern);
    if (pattern_len == 0u || pattern_len >= PRIV_CONFIG_DEFAULT_UFLOGGER_PATTERN_MAX) {
      return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
    }
  }

  /* A file destination with no path is a configuration that cannot be carried
     out.  Refusing it here is the difference between a caller learning at
     create time and a caller learning from an empty log file at 3am. */
  if (config_ptr->destination == UF_LOGGER_DESTINATION_FILE) {
    if (!config_ptr->file_path || config_ptr->file_path[0] == '\0') {
      return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
    }
    path_len = strlen(config_ptr->file_path);
    if (path_len >= (size_t)PRIV_CONFIG_DEFAULT_UFLOGGER_PATH_MAX) {
      return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
    }
  }

  rule_ptr = calloc(1u, sizeof(*rule_ptr));
  if (!rule_ptr) return UF_LOGGER_STATUS_ERR_NOMEM;

  /* The caller states one destination, so this is one rule.  It is an array
     because the routing model is general and the configuration-file front-end
     will populate more than one; the driver is written against the array
     already so that it does not have to change when that happens. */
  rule_ptr->match         = LOGGER_RULE_MATCH_ANY;
  rule_ptr->category      = NULL;
  rule_ptr->level_op      = LOGGER_RULE_LEVEL_AT_LEAST;
  rule_ptr->level         = level;
  rule_ptr->output_kind   = sDestinationToOutputKind(config_ptr->destination);
  rule_ptr->output_target = config_ptr->file_path;
  rule_ptr->syslog_facility = sFacilityToSyslog(config_ptr->syslog_facility);
  rule_ptr->format_pattern = (config_ptr->format_pattern && config_ptr->format_pattern[0])
                                 ? config_ptr->format_pattern
                                 : CONFIG_DEFAULT_UFLOGGER_FORMAT;

  rule_ptr->rotation.max_bytes = (config_ptr->max_file_bytes != 0u)
                                     ? config_ptr->max_file_bytes
                                     : (size_t)CONFIG_DEFAULT_UFLOGGER_MAX_FILE_BYTES;
  rule_ptr->rotation.max_archive_count = (config_ptr->max_archives != 0u)
                                             ? config_ptr->max_archives
                                             : CONFIG_DEFAULT_UFLOGGER_MAX_ARCHIVES;
  rule_ptr->rotation.archive_path_pattern = NULL;
  rule_ptr->rotation.sequence             = LOGGER_ARCHIVE_SEQUENCE_MONOTONIC;
  rule_ptr->rotation.sequence_digit_width = 0u;

  /* Left as the caller's pointer, or NULL when it named none.  The create path
     repoints it at the logger's own inline copy, because a compiled model that
     outlives the caller's string must not point at it. */
  out_config_ptr->category               = config_ptr->category;
  out_config_ptr->default_format_pattern = rule_ptr->format_pattern;
  out_config_ptr->level_map              = sLevelMap;
  out_config_ptr->level_map_count        = UFLOGGER_LEVEL_MAP_COUNT;
  out_config_ptr->rules                  = rule_ptr;
  out_config_ptr->rule_count             = 1u;

  out_config_ptr->settings.is_strict_init      = false;
  out_config_ptr->settings.buffer_min_bytes    = PRIV_CONFIG_DEFAULT_UFLOGGER_BUFFER_MIN_BYTES;
  out_config_ptr->settings.buffer_max_bytes    = PRIV_CONFIG_DEFAULT_UFLOGGER_BUFFER_MAX_BYTES;
  out_config_ptr->settings.file_perms          = PRIV_CONFIG_DEFAULT_UFLOGGER_FILE_PERMS;
  out_config_ptr->settings.rotate_lock_file_path = NULL;
  out_config_ptr->settings.reload_conf_period  = 0u;
  out_config_ptr->settings.fsync_period        = 0u;
  out_config_ptr->settings.is_async_writer     = false;
  out_config_ptr->settings.async_queue_length  = PRIV_CONFIG_DEFAULT_UFLOGGER_ASYNC_QUEUE_LENGTH;

  return UF_LOGGER_STATUS_OK;
}

void
LoggerCompiledConfigRelease(LoggerCompiledConfig *config_ptr)
{
  if (!config_ptr) return;

  /* The rule array is this module's only allocation inside a compiled config.
     Everything else — the category, the pattern, the level map — is borrowed
     from the caller or from a literal, and freeing one would be freeing memory
     this module never owned. */
  free((void *)config_ptr->rules);
  config_ptr->rules      = NULL;
  config_ptr->rule_count = 0u;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

UfLoggerStatus
LoggerCreateWithDriver(const UfLoggerConfig *config_ptr, const LoggerDriverTranslationTable *table_ptr,
                       UfLogger **out_logger_ptr)
{
  LoggerCompiledConfig compiled;
  UfLogger            *logger_ptr;
  UfLoggerStatus       status;
  const char          *category_ptr;
  size_t               category_len;

  if (!out_logger_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  *out_logger_ptr = NULL;

  if (!config_ptr || !table_ptr || !table_ptr->write || !table_ptr->open) {
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }

  status = LoggerCompileConfig(config_ptr, &compiled);
  if (status != UF_LOGGER_STATUS_OK) return status;

  /* A rotation size the caller asked for explicitly, against a build that
     cannot rotate, is refused rather than quietly dropped.  A size that came
     from the default is different: the caller expressed no preference, so
     falling back to no rotation loses nothing it asked for. */
  if (config_ptr->max_file_bytes != 0u &&
      (table_ptr->capabilities & UF_LOGGER_CAP_FILE_ROTATION) == 0u) {
    LoggerCompiledConfigRelease(&compiled);
    return UF_LOGGER_STATUS_ERR_CAPABILITY;
  }

  category_ptr = (config_ptr->category && config_ptr->category[0])
                     ? config_ptr->category
                     : CONFIG_DEFAULT_UFLOGGER_CATEGORY;
  category_len = strlen(category_ptr);
  if (category_len == 0u || category_len >= (size_t)PRIV_CONFIG_DEFAULT_UFLOGGER_CATEGORY_MAX) {
    LoggerCompiledConfigRelease(&compiled);
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }

  logger_ptr = calloc(1u, sizeof(*logger_ptr));
  if (!logger_ptr) {
    LoggerCompiledConfigRelease(&compiled);
    return UF_LOGGER_STATUS_ERR_NOMEM;
  }

  /* Copied inline rather than borrowed: the logger's identity has to be
     readable for its whole life, and a copy removes the question of whether the
     caller's string outlived its creator. */
  memcpy(logger_ptr->category, category_ptr, category_len);
  logger_ptr->category[category_len] = '\0';

  atomic_init(&logger_ptr->minimum_level, (int)compiled.rules[0].level);

  logger_ptr->compiled            = compiled;
  logger_ptr->compiled.category   = logger_ptr->category;
  logger_ptr->config              = *config_ptr;
  logger_ptr->driver.vtable       = table_ptr;
  logger_ptr->driver.state        = NULL;

  /* The backend is opened last, so that everything it might inspect is already
     in place.  It is also the step that can fail for reasons outside this
     module's control — an unwritable path, or a process-global resource another
     library already holds — which is why nothing before it is committed. */
  status = table_ptr->open(&logger_ptr->driver.state, &logger_ptr->compiled);
  if (status != UF_LOGGER_STATUS_OK) {
    free((void *)logger_ptr->compiled.rules);
    free(logger_ptr);
    return status;
  }

  *out_logger_ptr = logger_ptr;
  return UF_LOGGER_STATUS_OK;
}

UfLoggerStatus
UfLoggerCreate(const UfLoggerConfig *config_ptr, UfLogger **out_logger_ptr)
{
  const LoggerDriverTranslationTable *table_ptr = LoggerBuiltinDriverTable();

  /* No driver compiled in.  Reported rather than assumed away: a caller that
     reads the status can decide what to do, and one that did not check would
     have found out by crashing. */
  if (!table_ptr) {
    if (out_logger_ptr) *out_logger_ptr = NULL;
    return UF_LOGGER_STATUS_ERR_UNSUPPORTED;
  }

  return LoggerCreateWithDriver(config_ptr, table_ptr, out_logger_ptr);
}

UfLoggerStatus
UfLoggerCreateWithDefaults(UfLogger **out_logger_ptr)
{
  return UfLoggerCreate(UfLoggerProvideSaneDefaults(), out_logger_ptr);
}

const UfLoggerConfig *
UfLoggerProvideSaneDefaults(void)
{
  return &sSaneDefaults;
}

void
UfLoggerDestroy(UfLogger *logger_ptr)
{
  if (!logger_ptr) return;

  /* No closed-flag guard here, deliberately.  Such a flag could only ever be
     read after this function had already freed the handle it lives in, so the
     check would be the use-after-free rather than a protection against it.  A
     caller that destroys twice, or that emits through a destroyed handle, is
     already outside the contract; the public header says so plainly instead of
     offering a status code the library could not honestly return. */
  if (logger_ptr->driver.vtable && logger_ptr->driver.vtable->close) {
    logger_ptr->driver.vtable->close(logger_ptr->driver.state);
  }
  logger_ptr->driver.state = NULL;

  LoggerCompiledConfigRelease(&logger_ptr->compiled);
  free(logger_ptr);
}

/* ── The production binding ─────────────────────────────────────────────── */

/*!
 * The backend this build carries.
 *
 * Declared here rather than in a shared header because this function is its
 * only caller: a backend is reached through this table and never instantiated
 * by anything else, so a header would exist to serve one line.
 *
 * Compiled out with the backend it names.  A build without it links and returns
 * NULL, which @ref UfLoggerCreate reports as
 * @ref UF_LOGGER_STATUS_ERR_UNSUPPORTED — an honest answer rather than a logger
 * that accepts records and discards them.  The test is on the capability rather
 * than on a weak symbol, because a weak undefined reference does not pull its
 * definition out of a static archive: libuflib.a would leave this unresolved
 * even when the backend is in it.
 */
#if UFLIB_CAPABILITY_ZLOG
extern const LoggerDriverTranslationTable *LoggerZlogTranslationTable(void);
#endif

const LoggerDriverTranslationTable *
LoggerBuiltinDriverTable(void)
{
#if UFLIB_CAPABILITY_ZLOG
  return LoggerZlogTranslationTable();
#else
  return NULL;
#endif
}

/* ── Emit ───────────────────────────────────────────────────────────────── */

UfLoggerStatus
UfLoggerVLog(UfLogger *logger_ptr, UfLoggerLevel level, const char *file_ptr, int line,
             const char *function_ptr, const char *format_ptr, va_list args)
{
  char        stack_buffer[PRIV_CONFIG_DEFAULT_UFLOGGER_BUFFER_MIN_BYTES];
  char       *message_ptr = stack_buffer;
  char       *heap_ptr    = NULL;
  va_list     copy;
  int         needed;
  size_t      wanted;
  UfLoggerStatus status;

  if (!logger_ptr || !format_ptr || !sIsRealLevel(level)) {
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }

  /* Filtered before formatting, not after: the whole point of a severity floor
     is that a suppressed record costs a comparison rather than a format. */
  if ((int)level < atomic_load_explicit(&logger_ptr->minimum_level, memory_order_acquire)) {
    return UF_LOGGER_STATUS_OK;
  }

  va_copy(copy, args);
  needed = vsnprintf(stack_buffer, sizeof(stack_buffer), format_ptr, copy);
  va_end(copy);
  if (needed < 0) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;

  wanted = (size_t)needed + 1u;
  if (wanted > sizeof(stack_buffer)) {
    size_t ceiling = logger_ptr->compiled.settings.buffer_max_bytes;

    /* A record longer than the ceiling is truncated rather than dropped, and
       truncated visibly: a caller that wanted the whole thing can see that it
       did not get it, which a discarded record would never tell them.  The
       two-byte floor keeps the allocation able to hold a terminator and a
       character even if a ceiling were ever configured smaller than that. */
    if (ceiling != 0u && wanted > ceiling) wanted = ceiling;
    if (wanted < 2u) wanted = 2u;

    heap_ptr = malloc(wanted);
    if (!heap_ptr) return UF_LOGGER_STATUS_ERR_NOMEM;

    va_copy(copy, args);
    if (vsnprintf(heap_ptr, wanted, format_ptr, copy) < 0) {
      va_end(copy);
      free(heap_ptr);
      return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
    }
    va_end(copy);
    message_ptr = heap_ptr;
  }

  status = logger_ptr->driver.vtable->write(logger_ptr->driver.state, level, message_ptr,
                                            file_ptr, line, function_ptr);
  free(heap_ptr);
  return status;
}

UfLoggerStatus
UfLoggerLog(UfLogger *logger_ptr, UfLoggerLevel level, const char *file_ptr, int line,
            const char *function_ptr, const char *format_ptr, ...)
{
  va_list        args;
  UfLoggerStatus status;

  va_start(args, format_ptr);
  status = UfLoggerVLog(logger_ptr, level, file_ptr, line, function_ptr, format_ptr, args);
  va_end(args);
  return status;
}

#define UFLOGGER_DEFINE_LEVEL_FN(fn_name, level_value)                                        \
  UfLoggerStatus fn_name(UfLogger *logger_ptr, const char *file_ptr, int line,                 \
                         const char *function_ptr, const char *format_ptr, ...)                \
  {                                                                                            \
    va_list        args;                                                                       \
    UfLoggerStatus status;                                                                     \
    va_start(args, format_ptr);                                                                \
    status = UfLoggerVLog(logger_ptr, level_value, file_ptr, line, function_ptr, format_ptr,   \
                          args);                                                               \
    va_end(args);                                                                              \
    return status;                                                                             \
  }

UFLOGGER_DEFINE_LEVEL_FN(UfLoggerTrace,    UF_LOGGER_LEVEL_TRACE)
UFLOGGER_DEFINE_LEVEL_FN(UfLoggerDebug,    UF_LOGGER_LEVEL_DEBUG)
UFLOGGER_DEFINE_LEVEL_FN(UfLoggerInfo,     UF_LOGGER_LEVEL_INFO)
UFLOGGER_DEFINE_LEVEL_FN(UfLoggerNotice,   UF_LOGGER_LEVEL_NOTICE)
UFLOGGER_DEFINE_LEVEL_FN(UfLoggerWarn,     UF_LOGGER_LEVEL_WARN)
UFLOGGER_DEFINE_LEVEL_FN(UfLoggerError,    UF_LOGGER_LEVEL_ERROR)
UFLOGGER_DEFINE_LEVEL_FN(UfLoggerCritical, UF_LOGGER_LEVEL_CRITICAL)
UFLOGGER_DEFINE_LEVEL_FN(UfLoggerFatal,    UF_LOGGER_LEVEL_FATAL)

/* ── Severity control ───────────────────────────────────────────────────── */

UfLoggerStatus
UfLoggerSetLevel(UfLogger *logger_ptr, UfLoggerLevel level)
{
  UfLoggerStatus status;

  if (!logger_ptr || !sIsRealLevel(level)) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  if ((logger_ptr->driver.vtable->capabilities & UF_LOGGER_CAP_RUNTIME_LEVEL) == 0u ||
      !logger_ptr->driver.vtable->set_level) {
    return UF_LOGGER_STATUS_ERR_UNSUPPORTED;
  }

  /* The backend is told first.  A backend that refuses must leave the floor
     where it was, which it cannot do if the module has already moved it. */
  status = logger_ptr->driver.vtable->set_level(logger_ptr->driver.state, level);
  if (status != UF_LOGGER_STATUS_OK) return status;

  atomic_store_explicit(&logger_ptr->minimum_level, (int)level, memory_order_release);
  return UF_LOGGER_STATUS_OK;
}

UfLoggerLevel
UfLoggerGetLevel(const UfLogger *logger_ptr)
{
  if (!logger_ptr) return UF_LOGGER_LEVEL_FATAL;

  return (UfLoggerLevel)atomic_load_explicit(&logger_ptr->minimum_level, memory_order_acquire);
}

bool
UfLoggerIsLevelEnabled(const UfLogger *logger_ptr, UfLoggerLevel level)
{
  if (!logger_ptr || !sIsRealLevel(level)) return false;

  return (int)level >= atomic_load_explicit(&logger_ptr->minimum_level, memory_order_acquire);
}

/* ── Output control ─────────────────────────────────────────────────────── */

UfLoggerStatus
UfLoggerFlush(UfLogger *logger_ptr)
{
  if (!logger_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  if ((logger_ptr->driver.vtable->capabilities & UF_LOGGER_CAP_FLUSH) == 0u ||
      !logger_ptr->driver.vtable->flush) {
    return UF_LOGGER_STATUS_ERR_UNSUPPORTED;
  }

  return logger_ptr->driver.vtable->flush(logger_ptr->driver.state);
}

UfLoggerStatus
UfLoggerArchive(UfLogger *logger_ptr)
{
  if (!logger_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  if ((logger_ptr->driver.vtable->capabilities & UF_LOGGER_CAP_MANUAL_ARCHIVE) == 0u ||
      !logger_ptr->driver.vtable->archive) {
    return UF_LOGGER_STATUS_ERR_UNSUPPORTED;
  }

  return logger_ptr->driver.vtable->archive(logger_ptr->driver.state);
}

UfLoggerStatus
UfLoggerReload(UfLogger *logger_ptr, const UfLoggerConfig *config_ptr)
{
  const UfLoggerConfig *wanted_ptr;
  LoggerCompiledConfig  compiled;
  UfLoggerStatus        status;

  if (!logger_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  if ((logger_ptr->driver.vtable->capabilities & UF_LOGGER_CAP_RELOAD) == 0u ||
      !logger_ptr->driver.vtable->reload) {
    return UF_LOGGER_STATUS_ERR_UNSUPPORTED;
  }

  /* A NULL configuration means "re-apply what I already have", which is the
     useful reading of a reload with nothing new to say. */
  wanted_ptr = config_ptr ? config_ptr : &logger_ptr->config;

  status = LoggerCompileConfig(wanted_ptr, &compiled);
  if (status != UF_LOGGER_STATUS_OK) return status;

  status = logger_ptr->driver.vtable->reload(logger_ptr->driver.state, &compiled);
  if (status != UF_LOGGER_STATUS_OK) {
    LoggerCompiledConfigRelease(&compiled);
    return status;
  }

  /* Only once the backend has accepted the new model is the old one released:
     a rejected reload leaves the logger exactly as it was. */
  LoggerCompiledConfigRelease(&logger_ptr->compiled);
  logger_ptr->compiled          = compiled;
  logger_ptr->compiled.category = logger_ptr->category;
  logger_ptr->config            = *wanted_ptr;
  atomic_store_explicit(&logger_ptr->minimum_level, (int)compiled.rules[0].level,
                        memory_order_release);
  return UF_LOGGER_STATUS_OK;
}

/* ── Per-thread context ─────────────────────────────────────────────────── */

UfLoggerStatus
UfLoggerSetContext(UfLogger *logger_ptr, const char *key_ptr, const char *value_ptr)
{
  if (!logger_ptr || !key_ptr || !key_ptr[0] || !value_ptr) {
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }
  if (strlen(key_ptr) >= (size_t)PRIV_CONFIG_DEFAULT_UFLOGGER_CONTEXT_KEY_MAX ||
      strlen(value_ptr) >= (size_t)PRIV_CONFIG_DEFAULT_UFLOGGER_CONTEXT_VALUE_MAX) {
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }
  if ((logger_ptr->driver.vtable->capabilities & UF_LOGGER_CAP_CONTEXT) == 0u ||
      !logger_ptr->driver.vtable->set_context) {
    return UF_LOGGER_STATUS_ERR_UNSUPPORTED;
  }

  return logger_ptr->driver.vtable->set_context(logger_ptr->driver.state, key_ptr, value_ptr);
}

UfLoggerStatus
UfLoggerRemoveContext(UfLogger *logger_ptr, const char *key_ptr)
{
  if (!logger_ptr || !key_ptr || !key_ptr[0]) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  if ((logger_ptr->driver.vtable->capabilities & UF_LOGGER_CAP_CONTEXT) == 0u ||
      !logger_ptr->driver.vtable->remove_context) {
    return UF_LOGGER_STATUS_ERR_UNSUPPORTED;
  }

  return logger_ptr->driver.vtable->remove_context(logger_ptr->driver.state, key_ptr);
}

UfLoggerStatus
UfLoggerClearContext(UfLogger *logger_ptr)
{
  if (!logger_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  if ((logger_ptr->driver.vtable->capabilities & UF_LOGGER_CAP_CONTEXT) == 0u ||
      !logger_ptr->driver.vtable->clear_context) {
    return UF_LOGGER_STATUS_ERR_UNSUPPORTED;
  }

  return logger_ptr->driver.vtable->clear_context(logger_ptr->driver.state);
}

/* ── Introspection ──────────────────────────────────────────────────────── */

uint32_t
UfLoggerGetCapabilities(const UfLogger *logger_ptr)
{
  if (!logger_ptr || !logger_ptr->driver.vtable) return 0u;

  return logger_ptr->driver.vtable->capabilities;
}

/*!
 * Append a string as a JSON literal, escaping what must be escaped.
 *
 * The values described here include a path and a pattern, both of which come
 * from the caller and may contain a quote or a backslash.  Emitting one raw
 * would produce a document that no longer parses, which is a poor way for a
 * diagnostic to behave.
 */
static void
sAppendJsonString(BufferDescriptor *bd_ptr, const char *string_ptr)
{
  BufferDescriptorAppendFormatted(bd_ptr, "\"");

  for (const char *p = string_ptr; p && *p; p++) {
    switch (*p) {
    case '"':  BufferDescriptorAppendFormatted(bd_ptr, "\\\""); break;
    case '\\': BufferDescriptorAppendFormatted(bd_ptr, "\\\\"); break;
    case '\n': BufferDescriptorAppendFormatted(bd_ptr, "\\n");  break;
    case '\r': BufferDescriptorAppendFormatted(bd_ptr, "\\r");  break;
    case '\t': BufferDescriptorAppendFormatted(bd_ptr, "\\t");  break;
    default:   BufferDescriptorAppendFormatted(bd_ptr, "%c", *p); break;
    }
  }

  BufferDescriptorAppendFormatted(bd_ptr, "\"");
}

/*!
 * Append one capability name, separating it from what preceded it.
 *
 * @p emitted_ptr is threaded through rather than the array being joined
 * afterwards, because the mask decides which names appear and a caller reading
 * the document must not have to reason about a trailing comma that the mask
 * happened to produce.
 */
static void
sAppendCapabilityEntry(BufferDescriptor *bd_ptr, bool *emitted_ptr, const char *name_ptr)
{
  BufferDescriptorAppendFormatted(bd_ptr, *emitted_ptr ? ",\"%s\"" : "\"%s\"", name_ptr);
  *emitted_ptr = true;
}

static void
sAppendCapabilities(BufferDescriptor *bd_ptr, uint32_t capabilities)
{
  bool emitted = false;

  BufferDescriptorAppendFormatted(bd_ptr, "[");

  if (capabilities & UF_LOGGER_CAP_RUNTIME_LEVEL)  sAppendCapabilityEntry(bd_ptr, &emitted, "runtime_level");
  if (capabilities & UF_LOGGER_CAP_RELOAD)         sAppendCapabilityEntry(bd_ptr, &emitted, "reload");
  if (capabilities & UF_LOGGER_CAP_FLUSH)          sAppendCapabilityEntry(bd_ptr, &emitted, "flush");
  if (capabilities & UF_LOGGER_CAP_MANUAL_ARCHIVE) sAppendCapabilityEntry(bd_ptr, &emitted, "manual_archive");
  if (capabilities & UF_LOGGER_CAP_FILE_ROTATION)  sAppendCapabilityEntry(bd_ptr, &emitted, "file_rotation");
  if (capabilities & UF_LOGGER_CAP_CONTEXT)        sAppendCapabilityEntry(bd_ptr, &emitted, "context");

  BufferDescriptorAppendFormatted(bd_ptr, "]");
}

BufferDescriptor *
UfLoggerDescribeConfiguration(const UfLogger *logger_ptr, BufferDescriptor *provided_ptr)
{
  BufferDescriptor *bd_ptr = provided_ptr;
  bool              is_file_output;

  if (!bd_ptr) {
    bd_ptr = calloc(1u, sizeof(*bd_ptr));
    if (!bd_ptr) return NULL;
    BufferDescriptorInit(bd_ptr, 512u);
  }

  if (!logger_ptr) {
    BufferDescriptorAppendFormatted(bd_ptr, "{\"error\":\"null handle\"}\n");
    return bd_ptr;
  }

  BufferDescriptorAppendFormatted(bd_ptr, "{");

  BufferDescriptorAppendFormatted(bd_ptr, "\"category\":");
  sAppendJsonString(bd_ptr, logger_ptr->compiled.category);

  BufferDescriptorAppendFormatted(bd_ptr, ",\"minimum_level\":\"%s\"",
                                  sLevelName(UfLoggerGetLevel(logger_ptr)));
  BufferDescriptorAppendFormatted(bd_ptr, ",\"destination\":\"%s\"",
                                  sDestinationName(logger_ptr->config.destination));

  BufferDescriptorAppendFormatted(bd_ptr, ",\"file_path\":");
  if (logger_ptr->config.file_path) {
    sAppendJsonString(bd_ptr, logger_ptr->config.file_path);
  } else {
    BufferDescriptorAppendFormatted(bd_ptr, "null");
  }

  /* Rotation figures are reported only when there is a file to rotate.  The
     compiled rule always carries them, because they are defaulted rather than
     left unset — printing them against a syslog destination would describe
     something that is not happening, which is the opposite of what this
     function is for. */
  is_file_output = (logger_ptr->compiled.rules[0].output_kind == LOGGER_OUTPUT_FILE ||
                    logger_ptr->compiled.rules[0].output_kind == LOGGER_OUTPUT_FILE_SYNC);

  if (is_file_output) {
    BufferDescriptorAppendFormatted(bd_ptr, ",\"max_file_bytes\":%zu,\"max_archives\":%u",
                                    logger_ptr->compiled.rules[0].rotation.max_bytes,
                                    logger_ptr->compiled.rules[0].rotation.max_archive_count);
  } else {
    BufferDescriptorAppendFormatted(bd_ptr, ",\"max_file_bytes\":null,\"max_archives\":null");
  }

  BufferDescriptorAppendFormatted(bd_ptr, ",\"format_pattern\":");
  sAppendJsonString(bd_ptr, logger_ptr->compiled.rules[0].format_pattern);

  BufferDescriptorAppendFormatted(bd_ptr, ",\"capabilities\":");
  sAppendCapabilities(bd_ptr, UfLoggerGetCapabilities(logger_ptr));

  BufferDescriptorAppendFormatted(bd_ptr, "}\n");
  return bd_ptr;
}
