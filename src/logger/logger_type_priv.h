/**
 * @file logger_type_priv.h
 * @brief The logger's private types: the routing model, the driver seam, and the
 *        definition of the opaque handle.
 *
 * Not installed, and not reachable from a consumer.  It lives under @c src/,
 * which is on the library's PRIVATE include path only, and the install rule
 * copies @c include/ and nothing else — so a consumer has no include entry that
 * resolves it.  Within the library it is reached by quote-include from its
 * colocated sources, the same way every other module's @c _type_priv.h is.
 *
 * ## Why the superset lives here and not in the public header
 *
 * The module is a superset of what its backend can do: routing rules with four
 * kinds of severity predicate, seven output classes, a rank-to-backend severity
 * map, named formats, rotation and archive naming, buffering and asynchronous
 * writing.  A caller configures almost none of that — it states a category, a
 * floor, a destination and a shape, and the module compiles that down into the
 * model below.
 *
 * Keeping the model here is what makes the public interface small without
 * making the module less capable.  It is also the seam a configuration-file
 * front-end will feed later, which is why the model is expressed as data rather
 * than folded into the compiler of @ref UfLoggerConfig.
 *
 * ## Naming
 *
 * Types here carry no @c Uf prefix.  That prefix marks the public contract, and
 * nothing in this file is part of it — the distinction is deliberate and
 * visible at every use site.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_LOGGER_LOGGER_TYPE_PRIV_H
#define UFLIB_LOGGER_LOGGER_TYPE_PRIV_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/logger/logger_type.h>
#include <uflib/logger/logger_defs.h>

#include "logger_defs_priv.h"

/* ── Routing model ──────────────────────────────────────────────────────── */

/*!
 * How a rule's category selects what it applies to.
 *
 * The trailing-underscore prefix form is how a family of categories is covered
 * by one rule — @c "db_" matching @c db_read, @c db_write and @c db itself —
 * without the wildcard that would also catch @c dbx.  It is the mechanism the
 * public interface's "one logger per category" model is compiled into.
 */
typedef enum {
  LOGGER_RULE_MATCH_ANY = 0,  ///< Every category.
  LOGGER_RULE_MATCH_EXACT,    ///< One category, compared literally.
  LOGGER_RULE_MATCH_PREFIX,   ///< A category and everything below it in the @c _-separated family.
  LOGGER_RULE_MATCH_WASTEBIN  ///< Only when nothing else matched.
} LoggerRuleMatch;

/*!
 * How a rule's severity predicate compares an event.
 */
typedef enum {
  LOGGER_RULE_LEVEL_AT_LEAST = 0,  ///< level >= rule level.
  LOGGER_RULE_LEVEL_EQUAL,         ///< level == rule level.
  LOGGER_RULE_LEVEL_NOT_EQUAL,     ///< level != rule level.
  LOGGER_RULE_LEVEL_ANY            ///< No predicate.
} LoggerRuleLevelOp;

/*!
 * Where a rule sends what it captures.
 *
 * Wider than @ref UfLoggerDestination because the model is not limited to what
 * a caller configures: pipe and record outputs have no public counterpart yet,
 * and exist here so the model does not have to change when they gain one.
 */
typedef enum {
  LOGGER_OUTPUT_STDOUT = 0,
  LOGGER_OUTPUT_STDERR,
  LOGGER_OUTPUT_SYSLOG,
  LOGGER_OUTPUT_FILE,
  LOGGER_OUTPUT_FILE_SYNC,  ///< Every write is flushed to the device before returning.
  LOGGER_OUTPUT_PIPE,       ///< A child process, started with the rule's target as its command.
  LOGGER_OUTPUT_RECORD      ///< A callback registered under the rule's target name.
} LoggerRuleOutputKind;

/*!
 * How an archive path numbers the files it keeps.
 */
typedef enum {
  LOGGER_ARCHIVE_SEQUENCE_UNSET = 0,  ///< No archive path; the backend's default naming applies.
  LOGGER_ARCHIVE_SEQUENCE_ROLLING,    ///< Renumber the chain, oldest last.
  LOGGER_ARCHIVE_SEQUENCE_MONOTONIC   ///< Append the next number; existing archives are never renamed.
} LoggerArchiveSequence;

/*!
 * Rotation policy for a file output.
 *
 * A zeroed struct means no rotation.  Monotonic numbering is the intended
 * default for a configuration that names no scheme, because renumbering an
 * existing chain is the behaviour that loses data when a rotation is
 * interrupted.
 */
typedef struct {
  size_t                 max_bytes;
  uint32_t               max_archive_count;
  const char            *archive_path_pattern;
  LoggerArchiveSequence  sequence;
  uint8_t                sequence_digit_width;
} LoggerRotation;

/*!
 * One compiled routing rule.
 *
 * Rules are evaluated in array order and every match fires, so order is
 * meaningful wherever two rules overlap.
 */
typedef struct {
  LoggerRuleMatch       match;
  const char           *category;
  LoggerRuleLevelOp     level_op;
  UfLoggerLevel         level;
  LoggerRuleOutputKind  output_kind;
  const char           *output_target;
  int                   syslog_facility;
  LoggerRotation        rotation;
  const char           *format_pattern;
} LoggerRule;

/*!
 * One entry of the rank-to-backend severity map.
 *
 * The public interface carries ranks; the backend needs its own integers, its
 * own names and syslog severities.  This is where the two meet, and it is the
 * clearest example of why the map is private: a caller naming a backend integer
 * would be configuring the implementation.
 *
 * @c name is the spelling a backend's own configuration language uses.  It
 * belongs here rather than in each backend because a backend that spelled
 * severity differently would be describing the same eight ranks, and two
 * tables that must agree is one table too many.
 */
typedef struct {
  UfLoggerLevel level;         ///< The rank this entry defines.
  const char   *name;          ///< The backend-visible spelling of that rank.
  uint8_t       driver_level;  ///< Backend-side integer.
  int           syslog_level;  ///< syslog severity the rank reports as.
} LoggerLevelMapEntry;

/* ── Compiled configuration ─────────────────────────────────────────────── */

/*!
 * Process-wide settings, after defaults have been resolved.
 *
 * What @ref UfLoggerConfig's caller-facing fields compile into, together with
 * the tuning the caller never sees.
 */
typedef struct {
  bool          is_strict_init;
  size_t        buffer_min_bytes;
  size_t        buffer_max_bytes;
  uint32_t      file_perms;
  const char   *rotate_lock_file_path;
  uint32_t      reload_conf_period;
  uint32_t      fsync_period;
  bool          is_async_writer;
  size_t        async_queue_length;
} LoggerSettings;

/*!
 * A configuration compiled into the model the driver consumes.
 *
 * Produced from @ref UfLoggerConfig by the module, and from a configuration
 * file by the front-end that will follow.  Both paths produce this, which is
 * what lets the file format be added without the driver or the public
 * interface changing.
 */
typedef struct {
  const char               *category;
  const char               *default_format_pattern;
  LoggerSettings            settings;
  const LoggerLevelMapEntry *level_map;
  size_t                     level_map_count;
  const LoggerRule          *rules;
  size_t                     rule_count;
} LoggerCompiledConfig;

/* ── The driver seam ────────────────────────────────────────────────────── */

/*!
 * What a backend implements.
 *
 * One static table per backend, held by pointer on the driver.  Every slot a
 * backend cannot honour is NULL, and the capability mask says so in advance —
 * a caller tests the mask rather than discovering the gap by calling a slot
 * that was never bound.
 *
 * The table is static in the backend's own translation unit and outlives every
 * driver built from it.
 */
typedef struct {
  /*!
   * Bring the backend up against a compiled configuration.
   *
   * Receives the compiled model rather than the caller's struct, so a backend
   * never sees how the caller expressed itself.  Returns the backend's own
   * state through @p out_state_ptr, which every other slot receives.
   */
  UfLoggerStatus (*open)(void **out_state_ptr, const LoggerCompiledConfig *config_ptr);

  /*! Tear the backend down and release @p state_ptr. */
  void (*close)(void *state_ptr);

  /*!
   * Emit one already-formatted record.
   *
   * The caller has resolved severity and formatting by this point; a backend
   * that re-decides either would be second-guessing the module.
   */
  UfLoggerStatus (*write)(void *state_ptr, UfLoggerLevel level, const char *message_ptr,
                          const char *file_ptr, int line, const char *function_ptr);

  UfLoggerStatus (*set_level)(void *state_ptr, UfLoggerLevel level);
  UfLoggerStatus (*flush)(void *state_ptr);
  UfLoggerStatus (*archive)(void *state_ptr);
  UfLoggerStatus (*reload)(void *state_ptr, const LoggerCompiledConfig *config_ptr);

  /*!
   * Attach, remove and clear per-thread context.
   *
   * The backend owns this state rather than the core, because "per-thread"
   * means whatever the backend's threading model says it means — the baseline
   * keeps it in its own per-thread objects, and a backend that keeps it
   * elsewhere is equally valid.  A core-side store would have been a second
   * copy to keep in step with the one that actually renders the value.
   */
  UfLoggerStatus (*set_context)(void *state_ptr, const char *key_ptr, const char *value_ptr);
  UfLoggerStatus (*remove_context)(void *state_ptr, const char *key_ptr);
  UfLoggerStatus (*clear_context)(void *state_ptr);

  /*! What this backend honours, as @c UF_LOGGER_CAP_* bits. */
  uint32_t capabilities;
} LoggerDriverTranslationTable;

/*!
 * One instantiated backend.
 *
 * The split is the point: @c vtable is the backend's shared, static contract,
 * and @c state is that backend's own allocation whose layout this header
 * neither knows nor may know.
 */
typedef struct {
  const LoggerDriverTranslationTable *vtable;
  void                               *state;
} LoggerDriver;

/* ── The handle ─────────────────────────────────────────────────────────── */

/*!
 * @struct UfLogger
 * @brief The definition behind the public opaque handle.
 *
 * Only the module and its driver adapters include this file, so only they can
 * reach these members.
 *
 * @c category is held inline rather than as a borrowed pointer: the caller's
 * string must be readable for the logger's whole life, and a copy removes the
 * question of whether it outlived its creator.
 *
 * @c minimum_level is atomic because emitters read it on the hot path while a
 * reconfiguration may be writing it.  That makes the read safe; it does not
 * make @ref UfLoggerSetLevel safe to call concurrently with emitters, which the
 * public header states as a contract.
 *
 * @c compiled is the routing model the driver consumes.  Its @c category and
 * @c default_format_pattern point at @c category below and at a string literal
 * respectively — both are static-lifetime, so nothing needs freeing.  Its
 * @c rules, however, is allocated at create time, because the rule array is
 * compiled from the caller's configuration rather than being a literal, and a
 * general model must be able to hold more than one.  @ref UfLoggerDestroy is
 * what releases it.
 *
 * @c config keeps the caller's own values.  It exists so that a reload with no
 * new configuration, and the description emitted by
 * @ref UfLoggerDescribeConfiguration, can both report what the caller asked for
 * alongside what was made of it.
 *
 * @c driver owns @c driver.state; @ref UfLoggerDestroy is the only thing that
 * releases it, and only after the driver's own @c close slot has run.
 *
 * The exact member set settles as the implementation lands; what does not
 * change is that this definition is private, which is what keeps the public
 * interface from becoming a description of the implementation.
 */
struct UfLogger {
  char                    category[PRIV_CONFIG_DEFAULT_UFLOGGER_CATEGORY_MAX];
  _Atomic int             minimum_level;
  LoggerCompiledConfig    compiled;  ///< Routing model; @c rules is owned, the rest is borrowed.
  UfLoggerConfig          config;    ///< The caller's own values, kept for reload and description.
  LoggerDriver            driver;    ///< Bound backend; @c state is owned by this logger.
};

#endif /* UFLIB_LOGGER_LOGGER_TYPE_PRIV_H */
