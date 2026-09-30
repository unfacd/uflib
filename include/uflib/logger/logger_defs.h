/**
 * @file logger_defs.h
 * @brief The logger's public compile-time defaults.
 *
 * Only defaults a caller could reasonably want to know or override live here.
 * The module's own tuning — buffer sizing, name length limits, rule ceilings —
 * is private and lives in @c src/logger/logger_defs_priv.h, because a caller
 * has no decision to make about it.
 *
 * These are @c CONFIG_DEFAULT_* values and may be overridden at CMake configure
 * time through the generated @c config_uflib.h.  Each is re-declared under an
 * @c #ifndef guard so this header works whether or not that file is in scope;
 * the generated file remains the single override point.
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

#ifndef UFLIB_LOGGER_LOGGER_DEFS_H
#define UFLIB_LOGGER_LOGGER_DEFS_H

#include <uflib/logger/logger_type.h>

/*!
 * Category used when a configuration names none.
 *
 * The category is the identity a record carries and what a deployment routes
 * on, so a library logging on its own behalf is identifiable by default.
 */
#ifndef CONFIG_DEFAULT_UFLOGGER_CATEGORY
  #define CONFIG_DEFAULT_UFLOGGER_CATEGORY  "uflib"
#endif

/*!
 * Severity floor used when a configuration names none.
 *
 * INFO suppresses the tracing and developer detail a production server does not
 * want by default, while keeping everything an operator would act on.  A
 * caller wanting more turns it up explicitly at the call site it controls.
 */
#ifndef CONFIG_DEFAULT_UFLOGGER_LEVEL
  #define CONFIG_DEFAULT_UFLOGGER_LEVEL  UF_LOGGER_LEVEL_INFO
#endif

/*!
 * Syslog facility used when a configuration names none.
 *
 * @c LOG_USER is what every server in this fleet already passes to
 * @c openlog(3), so a logger created with defaults lands in the same place as
 * the logging it replaces.
 */
#ifndef CONFIG_DEFAULT_UFLOGGER_SYSLOG_FACILITY
  #define CONFIG_DEFAULT_UFLOGGER_SYSLOG_FACILITY  UF_LOGGER_SYSLOG_FACILITY_USER
#endif

/*!
 * Pattern applied when a configuration names none.
 *
 * Fields are: local time with milliseconds, severity, process and thread,
 * category, source file and line, then the message.  Millisecond precision and
 * the thread id are both present because the servers this serves are heavily
 * threaded and their existing lines carry thread identity explicitly; dropping
 * it would lose ordering information a reader depends on.
 *
 * The trailing @c %n is load-bearing — no backend appends a line terminator of
 * its own, so a pattern without it produces records that run together.
 */
#ifndef CONFIG_DEFAULT_UFLOGGER_FORMAT
  #define CONFIG_DEFAULT_UFLOGGER_FORMAT  "%d(%F %T).%ms %-8V [%p:%T] [%c] %f:%L %m%n"
#endif

/*!
 * Size at which a file destination rotates, when a configuration names none.
 *
 * Bounded rather than unbounded on purpose: a log file that can grow without
 * limit is an operational hazard, and a default that permits it would be a
 * default that eventually fills a disk.  Eight archives at this size bound a
 * destination to roughly 576 MiB.
 */
#ifndef CONFIG_DEFAULT_UFLOGGER_MAX_FILE_BYTES
  #define CONFIG_DEFAULT_UFLOGGER_MAX_FILE_BYTES  (64u * 1024u * 1024u)
#endif

/*!
 * Archives retained before the oldest is removed, when a configuration names
 * none.
 */
#ifndef CONFIG_DEFAULT_UFLOGGER_MAX_ARCHIVES
  #define CONFIG_DEFAULT_UFLOGGER_MAX_ARCHIVES  8u
#endif

#endif /* UFLIB_LOGGER_LOGGER_DEFS_H */
