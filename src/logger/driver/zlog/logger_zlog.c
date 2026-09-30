/**
 * @file logger_zlog.c
 * @brief The zlog backend: the module's baseline driver.
 *
 * This is the only file in uflib that includes zlog's header, and the only one
 * that names a zlog symbol.  Everything above it — the public interface, the
 * core, the routing model — is written without knowing this file exists.
 *
 * ## What it does
 *
 * It receives the compiled routing model and renders it as zlog configuration
 * text, then hands that text to zlog.  That is the whole of the translation:
 * the model's rules become zlog rules, its severity map becomes a zlog level
 * list, its formats become zlog formats.  No caller ever writes zlog syntax,
 * names a zlog category, or learns that zlog is involved.
 *
 * ## Why it owns the process-global state
 *
 * zlog is a process-global singleton whose initialisation is single-shot: a
 * second @c zlog_init() fails outright.  The core therefore does not know
 * whether a backend has process-global state at all — it simply propagates
 * whatever status @c open returns.  This file keeps the reference count, grows
 * and shrinks the shared configuration as loggers come and go, and is the only
 * place that knows zlog has to be initialised exactly once.
 *
 * ## What it refuses to claim
 *
 * zlog exposes no way to force a flush or a rotation, so this backend does not
 * claim @ref UF_LOGGER_CAP_FLUSH or @ref UF_LOGGER_CAP_MANUAL_ARCHIVE.  Those
 * calls therefore return @ref UF_LOGGER_STATUS_ERR_UNSUPPORTED because the mask
 * says so, rather than because an operation silently did nothing and reported
 * success.
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <pthread.h>

/* zlog's own header, reached through uflib's public include tree.  This is the
   only inclusion of it anywhere in the library. */
#include <uflib/zlog/zlog.h>

/* ── Limits ─────────────────────────────────────────────────────────────── */

/*!
 * Loggers this backend will hold at once.
 *
 * A backend shared by the whole process has to bound how many distinct
 * destinations it will carry, because each one costs a rule in one global
 * configuration.  Sixteen is far more than any single deployment here has
 * needed and small enough that the bound is real rather than nominal.
 */
#define ZLOG_MAX_LOGGERS 16u

/*! Room for the generated configuration text. */
#define ZLOG_CONFIG_CAPACITY 16384u

/*! Room for a generated format name, e.g. @c uflib_0. */
#define ZLOG_FORMAT_NAME_CAPACITY 32u

/*!
 * The path handed to zlog's @c rotate lock file setting.
 *
 * zlog opens this path and closes it; it takes no lock on it, so it provides no
 * exclusion between processes, and within a process the real guard is a mutex.
 * The path is therefore a token rather than a lock, and two consequences follow
 * from saying so plainly:
 *
 *   - left unset, zlog defaults to the *relative* path @c zlog-rotate.lock,
 *     which a daemon would create in whatever directory it happened to be
 *     started from — a stray file in the working directory, or a failure where
 *     that directory is not writable;
 *   - pointing it at a device that is always open succeeds without creating
 *     anything to clean up, and does not pretend to be a lock this backend
 *     cannot provide.
 *
 * The honest alternative — a real inter-process lock — is not something zlog
 * offers, which is why @ref UF_LOGGER_CAP_INTERPROCESS_LOCK does not exist.
 */
#define ZLOG_ROTATE_LOCK_PATH "/dev/null"

/* ── Per-logger state ───────────────────────────────────────────────────── */

/*!
 * One logger's share of the process-global zlog configuration.
 *
 * Everything a rule needs is copied in, because the compiled model a caller
 * passed is released once @c open returns, and because the configuration text
 * is regenerated from this array whenever the set of loggers changes.
 */
typedef struct {
  zlog_category_t     *category;
  char                 category_name[PRIV_CONFIG_DEFAULT_UFLOGGER_CATEGORY_MAX];
  char                 format_name[ZLOG_FORMAT_NAME_CAPACITY];
  char                 format_pattern[PRIV_CONFIG_DEFAULT_UFLOGGER_PATTERN_MAX];
  char                 output_target[PRIV_CONFIG_DEFAULT_UFLOGGER_PATH_MAX];
  uint8_t              level_to_zlog[UF_LOGGER_LEVEL_FATAL + 1u];
  UfLoggerLevel        minimum_level;
  LoggerRuleOutputKind output_kind;
  int                  syslog_facility;
  size_t               max_bytes;
  uint32_t             max_archives;
} LoggerZlogState;

/* ── Process-global state ───────────────────────────────────────────────── */

static pthread_mutex_t  s_lock = PTHREAD_MUTEX_INITIALIZER;
static LoggerZlogState *s_states[ZLOG_MAX_LOGGERS];
static unsigned         s_state_count;
static bool             s_is_initialised;

/* ── Text building ──────────────────────────────────────────────────────── */

/*!
 * Append to a bounded buffer, failing rather than truncating.
 *
 * A configuration truncated in silence would still parse — it would simply
 * configure less than it said — so running out of room has to be an error the
 * caller sees.
 */
static bool
sAppend(char *buffer_ptr, size_t capacity, size_t *offset_ptr, const char *format_ptr, ...)
{
  va_list args;
  int     written;

  if (*offset_ptr >= capacity) return false;

  va_start(args, format_ptr);
  written = vsnprintf(buffer_ptr + *offset_ptr, capacity - *offset_ptr, format_ptr, args);
  va_end(args);

  if (written < 0 || (size_t)written >= capacity - *offset_ptr) return false;

  *offset_ptr += (size_t)written;
  return true;
}

/*!
 * The spelling zlog's configuration uses for a syslog facility.
 *
 * This backend chooses the name rather than passing a caller's string through,
 * because zlog substitutes a facility of its own for one it does not recognise
 * instead of refusing it.  Naming only what is known removes that silent
 * fallback from the picture entirely — the value has already been validated
 * into an enumeration by the core, so there is nothing left to be surprised by.
 */
static const char *
sFacilityName(int facility)
{
  switch (facility) {
  case LOG_DAEMON: return "LOG_DAEMON";
  case LOG_LOCAL0: return "LOG_LOCAL0";
  case LOG_LOCAL1: return "LOG_LOCAL1";
  case LOG_LOCAL2: return "LOG_LOCAL2";
  case LOG_LOCAL3: return "LOG_LOCAL3";
  case LOG_LOCAL4: return "LOG_LOCAL4";
  case LOG_LOCAL5: return "LOG_LOCAL5";
  case LOG_LOCAL6: return "LOG_LOCAL6";
  case LOG_LOCAL7: return "LOG_LOCAL7";
  case LOG_USER:
  default:         return "LOG_USER";
  }
}

/*!
 * Render one logger's output clause.
 *
 * Rotation is attached only when there is a size to rotate at.  No archive
 * pattern is emitted: zlog requires that if one is given it carries a roll or
 * sequence token, and the interface offers a caller no way to express one, so
 * letting zlog keep its own archives beside the file is both simpler and safer
 * than inventing a scheme on the caller's behalf.
 */
static bool
sAppendOutput(char *buffer_ptr, size_t capacity, size_t *offset_ptr, const LoggerZlogState *state_ptr)
{
  const char *sync_prefix = (state_ptr->output_kind == LOGGER_OUTPUT_FILE_SYNC) ? "-" : "";

  switch (state_ptr->output_kind) {
  case LOGGER_OUTPUT_STDERR:
    return sAppend(buffer_ptr, capacity, offset_ptr, ">stderr");

  case LOGGER_OUTPUT_STDOUT:
    return sAppend(buffer_ptr, capacity, offset_ptr, ">stdout");

  case LOGGER_OUTPUT_FILE:
  case LOGGER_OUTPUT_FILE_SYNC:
    if (state_ptr->max_bytes == 0u) {
      return sAppend(buffer_ptr, capacity, offset_ptr, "%s\"%s\"", sync_prefix, state_ptr->output_target);
    }
    return sAppend(buffer_ptr, capacity, offset_ptr, "%s\"%s\", %zu * %u",
                   sync_prefix, state_ptr->output_target, state_ptr->max_bytes,
                   state_ptr->max_archives);

  case LOGGER_OUTPUT_SYSLOG:
  default:
    return sAppend(buffer_ptr, capacity, offset_ptr, ">syslog, %s",
                   sFacilityName(state_ptr->syslog_facility));
  }
}

/*!
 * Render the whole process configuration from the set of live loggers.
 *
 * One global configuration carries every logger's rules, which is why it is
 * rebuilt whenever one is added or removed rather than built once and kept.
 *
 * The @c [levels] block is unconditional.  zlog's built-in severities have no
 * TRACE and no CRITICAL, and this module's rank model contains both; defining
 * them here is what makes a rank's integer mean the same thing to zlog as it
 * does to the module.  A build that omitted it would have those two ranks fall
 * back to zlog's "unknown" severity, silently.
 */
static bool
sBuildConfigText(char *buffer_ptr, size_t capacity, size_t *length_ptr)
{
  size_t offset = 0u;

  if (!sAppend(buffer_ptr, capacity, &offset,
               "[global]\n"
               "strict init = false\n"
               "buffer min = %u\n"
               "buffer max = %u\n"
               "file perms = %o\n"
               "fsync period = 0\n"
               "rotate lock file = " ZLOG_ROTATE_LOCK_PATH "\n"
               "\n[levels]\n"
               "TRACE = 10, LOG_DEBUG\n"
               "CRITICAL = 110, LOG_CRIT\n"
               "\n[formats]\n",
               (unsigned)PRIV_CONFIG_DEFAULT_UFLOGGER_BUFFER_MIN_BYTES,
               (unsigned)PRIV_CONFIG_DEFAULT_UFLOGGER_BUFFER_MAX_BYTES,
               (unsigned)PRIV_CONFIG_DEFAULT_UFLOGGER_FILE_PERMS)) {
    return false;
  }

  for (unsigned i = 0u; i < s_state_count; i++) {
    if (!sAppend(buffer_ptr, capacity, &offset, "%s = \"%s\"\n",
                 s_states[i]->format_name, s_states[i]->format_pattern)) {
      return false;
    }
  }

  if (!sAppend(buffer_ptr, capacity, &offset, "\n[rules]\n")) return false;

  for (unsigned i = 0u; i < s_state_count; i++) {
    const LoggerZlogState *state_ptr = s_states[i];

    /* The rule matches every severity, deliberately.  A rule naming a level
       imposes that level as a permanent floor of its own, and no amount of
       per-category level switching can widen past it — so a logger whose floor
       a caller later lowered would accept the call, report success, and drop
       every record below the rule's level.  Leaving the predicate open makes
       the module's own floor the single authority on severity, which is what
       it already is: the core filters before it ever reaches this backend. */
    if (!sAppend(buffer_ptr, capacity, &offset, "%s.*  ", state_ptr->category_name)) {
      return false;
    }
    if (!sAppendOutput(buffer_ptr, capacity, &offset, state_ptr)) return false;
    if (!sAppend(buffer_ptr, capacity, &offset, "; %s\n", state_ptr->format_name)) return false;
  }

  *length_ptr = offset;
  return true;
}

/* ── Capture ────────────────────────────────────────────────────────────── */

/*!
 * Copy everything a rule needs out of the compiled model.
 *
 * Returns false when the model holds something this backend cannot render
 * faithfully.  A pattern carrying a quote or a newline would end the
 * configuration string early and corrupt every rule after it, so it is refused
 * rather than written.
 */
static bool
sCaptureState(LoggerZlogState *state_ptr, const LoggerCompiledConfig *config_ptr, unsigned index)
{
  const LoggerRule *rule_ptr;

  if (config_ptr->rule_count == 0u || !config_ptr->category) return false;

  rule_ptr = &config_ptr->rules[0];
  if (!rule_ptr->format_pattern) return false;

  if (strlen(config_ptr->category) >= sizeof(state_ptr->category_name)) return false;
  if (strlen(rule_ptr->format_pattern) >= sizeof(state_ptr->format_pattern)) return false;
  if (strchr(rule_ptr->format_pattern, '"') || strchr(rule_ptr->format_pattern, '\n')) return false;

  if (rule_ptr->output_target) {
    if (strlen(rule_ptr->output_target) >= sizeof(state_ptr->output_target)) return false;
    if (strchr(rule_ptr->output_target, '"') || strchr(rule_ptr->output_target, '\n')) return false;
    snprintf(state_ptr->output_target, sizeof(state_ptr->output_target), "%s", rule_ptr->output_target);
  }

  /* The rank-to-integer map is copied, not borrowed: the configuration text is
     regenerated long after the compiled model it came from has been released. */
  for (size_t i = 0u; i < config_ptr->level_map_count; i++) {
    UfLoggerLevel rank = config_ptr->level_map[i].level;

    if ((unsigned)rank > UF_LOGGER_LEVEL_FATAL) continue;
    state_ptr->level_to_zlog[rank] = config_ptr->level_map[i].driver_level;
  }

  /* The rule's own rank must have an integer to be written with.  Without one
     the backend would have to invent a severity, and a record emitted at a
     severity other than the one asked for is worse than a refused create. */
  if ((unsigned)rule_ptr->level > UF_LOGGER_LEVEL_FATAL ||
      state_ptr->level_to_zlog[rule_ptr->level] == 0u) {
    return false;
  }

  snprintf(state_ptr->category_name, sizeof(state_ptr->category_name), "%s", config_ptr->category);
  snprintf(state_ptr->format_pattern, sizeof(state_ptr->format_pattern), "%s", rule_ptr->format_pattern);
  snprintf(state_ptr->format_name, sizeof(state_ptr->format_name), "uflib_%u", index);

  state_ptr->minimum_level   = rule_ptr->level;
  state_ptr->output_kind     = rule_ptr->output_kind;
  state_ptr->syslog_facility = rule_ptr->syslog_facility;
  state_ptr->max_bytes       = (rule_ptr->output_kind == LOGGER_OUTPUT_FILE ||
                                rule_ptr->output_kind == LOGGER_OUTPUT_FILE_SYNC)
                                   ? rule_ptr->rotation.max_bytes : 0u;
  state_ptr->max_archives    = rule_ptr->rotation.max_archive_count;
  return true;
}

/*!
 * Install the current set of loggers into zlog, first time or subsequently.
 *
 * Called with @ref s_lock held.
 */
static int
sApplyConfigText(char *buffer_ptr, size_t capacity)
{
  size_t length = 0u;

  if (!sBuildConfigText(buffer_ptr, capacity, &length)) return -1;

  if (!s_is_initialised) {
    if (zlog_init_from_string(buffer_ptr) != 0) return -1;
    s_is_initialised = true;
    return 0;
  }

  return zlog_reload_from_string(buffer_ptr);
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

static UfLoggerStatus
zOpen(void **out_state_ptr, const LoggerCompiledConfig *config_ptr)
{
  static char      config_text[ZLOG_CONFIG_CAPACITY];
  LoggerZlogState *state_ptr;
  UfLoggerStatus   status = UF_LOGGER_STATUS_OK;

  if (!out_state_ptr || !config_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  *out_state_ptr = NULL;

  state_ptr = calloc(1u, sizeof(*state_ptr));
  if (!state_ptr) return UF_LOGGER_STATUS_ERR_NOMEM;

  pthread_mutex_lock(&s_lock);

  /* Captured under the lock, because the format name it generates carries the
     slot index and the slot index is what the lock protects. */
  if (!sCaptureState(state_ptr, config_ptr, s_state_count)) {
    status = UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
    goto done;
  }

  if (s_state_count >= ZLOG_MAX_LOGGERS) {
    status = UF_LOGGER_STATUS_ERR_BUSY;
    goto done;
  }

  s_states[s_state_count++] = state_ptr;

  if (sApplyConfigText(config_text, sizeof(config_text)) != 0) {
    /* A first failure here is almost always another part of the process already
       holding zlog: its initialisation is single-shot, and the configuration
       this module would install cannot coexist with one already in place.
       Reported as busy rather than as a fault, because the caller's remedy is
       to find the other owner, not to retry. */
    s_state_count--;
    s_states[s_state_count] = NULL;
    status = s_is_initialised ? UF_LOGGER_STATUS_ERR_BACKEND : UF_LOGGER_STATUS_ERR_BUSY;
    goto done;
  }

  state_ptr->category = zlog_get_category(state_ptr->category_name);
  if (!state_ptr->category) {
    s_state_count--;
    s_states[s_state_count] = NULL;
    status = UF_LOGGER_STATUS_ERR_BACKEND;
    goto done;
  }

  /* Narrow zlog's own view to the module's floor.  The rule matches every
     severity so that the floor can move later; this is what keeps a suppressed
     record from doing work inside zlog before the module's gate is reached. */
  zlog_level_switch(state_ptr->category,
                    (int)state_ptr->level_to_zlog[state_ptr->minimum_level]);

done:
  pthread_mutex_unlock(&s_lock);
  if (status != UF_LOGGER_STATUS_OK) {
    free(state_ptr);
    return status;
  }

  *out_state_ptr = state_ptr;
  return UF_LOGGER_STATUS_OK;
}

static void
zClose(void *state_ptr)
{
  LoggerZlogState *closing_ptr = state_ptr;

  if (!closing_ptr) return;

  pthread_mutex_lock(&s_lock);

  for (unsigned i = 0u; i < s_state_count; i++) {
    if (s_states[i] != closing_ptr) continue;
    for (unsigned j = i; j + 1u < s_state_count; j++) s_states[j] = s_states[j + 1u];
    s_states[--s_state_count] = NULL;
    break;
  }

  /* The last logger out releases zlog itself.  Leaving a process-global logging
     library initialised after the last thing using it has gone would be holding
     a resource on behalf of nobody, and it is what would make a later logger's
     initialisation fail against a configuration no longer anyone's.

     The departed logger's rules are left in place otherwise: they name a
     category nothing writes to any more, and regenerating the whole
     configuration on every close would rewrite every other logger's rules to
     no purpose. */
  if (s_state_count == 0u && s_is_initialised) {
    zlog_fini();
    s_is_initialised = false;
  }

  pthread_mutex_unlock(&s_lock);
  free(closing_ptr);
}

/* ── Records ────────────────────────────────────────────────────────────── */

static UfLoggerStatus
zWrite(void *state_ptr, UfLoggerLevel level, const char *message_ptr,
       const char *file_ptr, int line, const char *function_ptr)
{
  LoggerZlogState *writing_ptr = state_ptr;

  /* The module validates the handle before calling in, so reaching here means the
     backend's own state is unusable — reported as a backend failure rather than
     as a lifecycle state the caller could act on. */
  if (!writing_ptr || !writing_ptr->category) return UF_LOGGER_STATUS_ERR_BACKEND;
  if ((unsigned)level > UF_LOGGER_LEVEL_FATAL || level == UF_LOGGER_LEVEL_DEFAULT) {
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }

  /* zlog's variadic entry point rather than one of its severity macros: the
     macros would substitute this file's own source location for the caller's,
     and carrying the caller's location all the way down is the point. */
  zlog(writing_ptr->category,
       file_ptr ? file_ptr : "", file_ptr ? strlen(file_ptr) : 0u,
       function_ptr ? function_ptr : "", function_ptr ? strlen(function_ptr) : 0u,
       (long)line, (int)writing_ptr->level_to_zlog[level], "%s",
       message_ptr ? message_ptr : "");

  return UF_LOGGER_STATUS_OK;
}

/* ── Control ────────────────────────────────────────────────────────────── */

static UfLoggerStatus
zSetLevel(void *state_ptr, UfLoggerLevel level)
{
  LoggerZlogState *setting_ptr = state_ptr;

  if (!setting_ptr || !setting_ptr->category) return UF_LOGGER_STATUS_ERR_BACKEND;
  if ((unsigned)level > UF_LOGGER_LEVEL_FATAL || level == UF_LOGGER_LEVEL_DEFAULT) {
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }

  /* zlog documents this as not thread-safe, so it is serialised here.  The
     module's own floor is what actually filters; narrowing zlog's rule to match
     keeps the two agreeing, so a record that reached zlog is not dropped there
     for a reason the module has already accounted for. */
  pthread_mutex_lock(&s_lock);
  zlog_level_switch(setting_ptr->category, (int)setting_ptr->level_to_zlog[level]);
  pthread_mutex_unlock(&s_lock);

  return UF_LOGGER_STATUS_OK;
}

static UfLoggerStatus
zReload(void *state_ptr, const LoggerCompiledConfig *config_ptr)
{
  static char      config_text[ZLOG_CONFIG_CAPACITY];
  LoggerZlogState *live_ptr = state_ptr;
  LoggerZlogState  staged;
  unsigned         index = 0u;
  size_t           length = 0u;
  bool             found = false;

  if (!live_ptr || !config_ptr) return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;

  pthread_mutex_lock(&s_lock);

  for (unsigned i = 0u; i < s_state_count; i++) {
    if (s_states[i] == live_ptr) { index = i; found = true; break; }
  }
  if (!found) {
    /* Not in this backend's registry: its state is unusable for this logger. */
    pthread_mutex_unlock(&s_lock);
    return UF_LOGGER_STATUS_ERR_BACKEND;
  }

  /* Staged into a scratch copy first: a configuration that cannot be rendered
     must leave the live logger untouched, and rewriting it in place would have
     spoiled it before that was discovered. */
  memset(&staged, 0, sizeof(staged));
  if (!sCaptureState(&staged, config_ptr, index)) {
    pthread_mutex_unlock(&s_lock);
    return UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT;
  }
  staged.category = live_ptr->category;

  *live_ptr = staged;

  if (!sBuildConfigText(config_text, sizeof(config_text), &length) ||
      zlog_reload_from_string(config_text) != 0) {
    pthread_mutex_unlock(&s_lock);
    return UF_LOGGER_STATUS_ERR_BACKEND;
  }

  /* A reload recomputes every category's rule set, and with it the per-category
     severity zlog_level_switch had installed.  Re-applying the module's floor
     keeps the two views agreeing after the reload rather than only until it. */
  zlog_level_switch(live_ptr->category, (int)live_ptr->level_to_zlog[live_ptr->minimum_level]);

  pthread_mutex_unlock(&s_lock);
  return UF_LOGGER_STATUS_OK;
}

/* ── Per-thread context ─────────────────────────────────────────────────── */

static UfLoggerStatus
zSetContext(void *state_ptr, const char *key_ptr, const char *value_ptr)
{
  (void)state_ptr;

  return (zlog_put_mdc(key_ptr, value_ptr) == 0) ? UF_LOGGER_STATUS_OK
                                                 : UF_LOGGER_STATUS_ERR_BACKEND;
}

static UfLoggerStatus
zRemoveContext(void *state_ptr, const char *key_ptr)
{
  (void)state_ptr;
  zlog_remove_mdc(key_ptr);
  return UF_LOGGER_STATUS_OK;
}

static UfLoggerStatus
zClearContext(void *state_ptr)
{
  (void)state_ptr;
  zlog_clean_mdc();
  return UF_LOGGER_STATUS_OK;
}

/* ── The table ──────────────────────────────────────────────────────────── */

static const LoggerDriverTranslationTable sZlogTable = {
  .open           = zOpen,
  .close          = zClose,
  .write          = zWrite,
  .set_level      = zSetLevel,
  .flush          = NULL,  /* zlog exposes no flush; not claimed below.      */
  .archive        = NULL,  /* zlog exposes no manual rotate; not claimed.    */
  .reload         = zReload,
  .set_context    = zSetContext,
  .remove_context = zRemoveContext,
  .clear_context  = zClearContext,

  /* FILE_ROTATION is claimed because this backend does emit a rotation clause.
     FLUSH and MANUAL_ARCHIVE are not, because zlog offers no way to honour
     them — and saying so in advance is what lets the core answer honestly
     instead of reporting a success it did not achieve. */
  .capabilities   = UF_LOGGER_CAP_RUNTIME_LEVEL | UF_LOGGER_CAP_RELOAD |
                    UF_LOGGER_CAP_FILE_ROTATION | UF_LOGGER_CAP_CONTEXT
};

const LoggerDriverTranslationTable *
LoggerZlogTranslationTable(void)
{
  return &sZlogTable;
}
