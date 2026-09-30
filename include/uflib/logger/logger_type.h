/**
 * @file logger_type.h
 * @brief The logger's public types: an opaque handle, the values a caller
 *        configures it with, and nothing else.
 *
 * This header is type-only.  It carries no function declarations and no
 * implementation state — those are in logger.h and in the private
 * @c src/logger/logger_type_priv.h respectively.
 *
 * ## What a caller is given, and what it is not
 *
 * A caller is given a handle it cannot look inside, and a configuration struct
 * describing what the logger should do.  It is not given the routing model, the
 * severity-to-backend mapping, the buffer sizing, the archive naming scheme, or
 * any other thing that describes how the logger is built.  Those exist — the
 * module is a superset of what its backend can do, and always will be — but
 * they are the module's business, not the caller's.
 *
 * The test applied to every field below is: *is this the caller's decision, or
 * the implementation's?*  A severity floor is the caller's.  A rank-to-integer
 * mapping is not.  A destination is the caller's.  A queue depth is not.
 *
 * ## Zero means default
 *
 * A field left zero accepts the module's default.  @ref UfLoggerLevel therefore
 * reserves 0 as an explicit "unset" marker, so that a zeroed configuration
 * cannot be mistaken for one that asked to log everything.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UFLIB_LOGGER_LOGGER_TYPE_H
#define UFLIB_LOGGER_LOGGER_TYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handle ──────────────────────────────────────────────────────── */

/*!
 * A logger instance.
 *
 * Named here, defined in @c src/logger/logger_type_priv.h.  A caller creates
 * one, logs through it, and destroys it.  It never allocates one, never reads a
 * member, and never learns what is behind it.
 */
typedef struct UfLogger UfLogger;

/* ── Status ─────────────────────────────────────────────────────────────── */

/*!
 * Outcome of a logger operation.
 *
 * A record suppressed by policy is @ref UF_LOGGER_STATUS_OK: the policy handled
 * it and no output was wanted.  Only a failure to carry the policy out is an
 * error.
 *
 * ## Every value here is reachable
 *
 * There is deliberately no code for "this logger has already been destroyed".
 * @ref UfLoggerDestroy frees the handle, so a pointer to it cannot afterwards
 * be inspected to discover that it was freed — a code claiming to report that
 * could never be returned honestly, and offering one would suggest the library
 * was checking when it cannot be.  Use of a handle after destruction is
 * undefined; see the lifetime note in logger.h.
 */
typedef enum UfLoggerStatus {
  UF_LOGGER_STATUS_OK = 0,                    ///< Carried out as configured.
  UF_LOGGER_STATUS_ERR_INVALID_ARGUMENT = -1, ///< A NULL, empty or out-of-range argument.
  UF_LOGGER_STATUS_ERR_NOMEM = -2,            ///< Allocation failed.
  UF_LOGGER_STATUS_ERR_IO = -3,               ///< A configured destination could not be opened.
  UF_LOGGER_STATUS_ERR_UNSUPPORTED = -4,      ///< Not honoured by this build; check the capability mask.
  UF_LOGGER_STATUS_ERR_BACKEND = -5,          ///< Accepted and then failed, or the backend's state is unusable.
  UF_LOGGER_STATUS_ERR_BUSY = -6,             ///< A process-global resource is held by another owner, or the logger ceiling was reached.
  UF_LOGGER_STATUS_ERR_CAPABILITY = -7        ///< The configuration asks for something this build cannot do.
} UfLoggerStatus;

/* ── Severity ───────────────────────────────────────────────────────────── */

/*!
 * Severity, as an ordered rank.
 *
 * The values are ranks, not any backend's numbers.  A caller compares two
 * levels and names one; it never depends on the integer a backend assigns,
 * because that integer is an implementation detail the module owns.
 *
 * @ref UF_LOGGER_LEVEL_DEFAULT is 0 and is not a level.  It exists so a zeroed
 * configuration means "unspecified" rather than TRACE.
 */
typedef enum UfLoggerLevel {
  UF_LOGGER_LEVEL_DEFAULT = 0,  ///< Unset — the module's default severity applies.
  UF_LOGGER_LEVEL_TRACE,        ///< Finest-grained tracing.
  UF_LOGGER_LEVEL_DEBUG,        ///< Developer-facing detail.
  UF_LOGGER_LEVEL_INFO,         ///< Normal operational milestones.
  UF_LOGGER_LEVEL_NOTICE,       ///< Unusual but not a fault.
  UF_LOGGER_LEVEL_WARN,         ///< A condition that may become a fault.
  UF_LOGGER_LEVEL_ERROR,        ///< An operation failed.
  UF_LOGGER_LEVEL_CRITICAL,     ///< A subsystem is impaired.
  UF_LOGGER_LEVEL_FATAL         ///< The process cannot continue.
} UfLoggerLevel;

/* ── Destination ────────────────────────────────────────────────────────── */

/*!
 * Where a logger sends its records.
 *
 * One logger writes to one destination.  A component that needs its records in
 * two places uses two loggers, each with its own category — which is also what
 * keeps a category's identity and its destination from having to be described
 * separately.
 */
typedef enum UfLoggerDestination {
  UF_LOGGER_DESTINATION_SYSLOG = 0,  ///< Host syslog; the default, and what the servers use today.
  UF_LOGGER_DESTINATION_STDERR,      ///< Process standard error.
  UF_LOGGER_DESTINATION_STDOUT,      ///< Process standard output.
  UF_LOGGER_DESTINATION_FILE         ///< A file, bounded by @ref UfLoggerConfig.max_file_bytes.
} UfLoggerDestination;

/*!
 * Syslog facility, when the destination is syslog.
 *
 * The facility decides which syslog routing rule a line lands under, so it is a
 * deployment choice and therefore the caller's.  The set is the facilities a
 * daemon realistically logs as, not every facility syslog defines.
 */
typedef enum UfLoggerSyslogFacility {
  UF_LOGGER_SYSLOG_FACILITY_DEFAULT = 0,  ///< Unset — the module's default facility applies.
  UF_LOGGER_SYSLOG_FACILITY_USER,         ///< General user-level messages; the historical default here.
  UF_LOGGER_SYSLOG_FACILITY_DAEMON,       ///< System daemons.
  UF_LOGGER_SYSLOG_FACILITY_LOCAL0,       ///< Reserved for site-local use.
  UF_LOGGER_SYSLOG_FACILITY_LOCAL1,
  UF_LOGGER_SYSLOG_FACILITY_LOCAL2,
  UF_LOGGER_SYSLOG_FACILITY_LOCAL3,
  UF_LOGGER_SYSLOG_FACILITY_LOCAL4,
  UF_LOGGER_SYSLOG_FACILITY_LOCAL5,
  UF_LOGGER_SYSLOG_FACILITY_LOCAL6,
  UF_LOGGER_SYSLOG_FACILITY_LOCAL7
} UfLoggerSyslogFacility;

/* ── Configuration ──────────────────────────────────────────────────────── */

/*!
 * What a caller tells the logger to do.
 *
 * Every field here is a decision a caller is entitled to make.  Every field
 * left zero accepts the module's default, so a zeroed struct is valid.
 *
 * Strings are **borrowed, not copied**, and must outlive the logger.
 *
 * @code{.c}
 * UfLoggerConfig config = {
 *     .category        = "orders",
 *     .minimum_level   = UF_LOGGER_LEVEL_INFO,
 *     .destination     = UF_LOGGER_DESTINATION_FILE,
 *     .file_path       = "/var/log/ufsrv/orders.log",
 *     .max_file_bytes  = 64u * 1024u * 1024u,
 *     .max_archives    = 8u,
 * };
 * @endcode
 */
typedef struct UfLoggerConfig {
  const char            *category;        ///< Identity records carry; NULL → @c CONFIG_DEFAULT_UFLOGGER_CATEGORY.
  UfLoggerLevel          minimum_level;   ///< Severity floor; DEFAULT → @c CONFIG_DEFAULT_UFLOGGER_LEVEL.
  UfLoggerDestination    destination;     ///< Where records go.

  /* File destination only; ignored otherwise. */
  const char            *file_path;       ///< Path to write; required when the destination is FILE.
  size_t                 max_file_bytes;  ///< Rotate at this size; 0 → @c CONFIG_DEFAULT_UFLOGGER_MAX_FILE_BYTES.
  uint32_t               max_archives;    ///< Archives to retain; 0 → @c CONFIG_DEFAULT_UFLOGGER_MAX_ARCHIVES.

  /* Syslog destination only; ignored otherwise. */
  UfLoggerSyslogFacility syslog_facility; ///< Facility to log under; DEFAULT → @c CONFIG_DEFAULT_UFLOGGER_SYSLOG_FACILITY.

  const char            *format_pattern;  ///< Output shape; NULL → @c CONFIG_DEFAULT_UFLOGGER_FORMAT.
} UfLoggerConfig;

/* ── Capabilities ───────────────────────────────────────────────────────── */

/*
 * What this build can honour, read through @ref UfLoggerGetCapabilities.
 *
 * A caller tests a bit before calling the operation it guards, so that an
 * unsupported request is answered honestly rather than accepted and ignored.
 * These are the operations a caller can invoke — not a description of how the
 * logger is built, which is not the caller's concern.
 */

#define UF_LOGGER_CAP_RUNTIME_LEVEL  (1u << 0)  ///< The severity floor can be changed after creation.
#define UF_LOGGER_CAP_RELOAD         (1u << 1)  ///< The configuration can be replaced at runtime.
#define UF_LOGGER_CAP_FLUSH          (1u << 2)  ///< Buffered records can be forced out.
#define UF_LOGGER_CAP_MANUAL_ARCHIVE (1u << 3)  ///< An archive can be forced.
#define UF_LOGGER_CAP_FILE_ROTATION  (1u << 4)  ///< A file destination honours max_file_bytes.
#define UF_LOGGER_CAP_CONTEXT        (1u << 5)  ///< Per-thread context can be attached and rendered.

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_LOGGER_LOGGER_TYPE_H */
