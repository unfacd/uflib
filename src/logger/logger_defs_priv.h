/**
 * @file logger_defs_priv.h
 * @brief The logger module's private compile-time constants.
 *
 * Not installed, and not reachable from a consumer: this file lives under
 * @c src/, which is on the library's PRIVATE include path only, and the install
 * rule copies @c include/ and nothing else.  A consumer has no include entry
 * that resolves it.
 *
 * Everything here bounds or tunes the implementation.  None of it is a
 * decision a caller is entitled to make, which is exactly why it is not in the
 * public @ref logger_defs.h — a caller that could size the message buffer or
 * raise the rule ceiling would be reasoning about the implementation rather
 * than about logging.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_LOGGER_LOGGER_DEFS_PRIV_H
#define UFLIB_LOGGER_LOGGER_DEFS_PRIV_H

/*!
 * Lower bound of the per-thread record buffer, in bytes.
 *
 * A formatted record is assembled here before it reaches a destination.  The
 * floor keeps a short record from costing an allocation on every call.
 */
#define PRIV_CONFIG_DEFAULT_UFLOGGER_BUFFER_MIN_BYTES  1024U

/*!
 * Upper bound of the per-thread record buffer, in bytes.
 *
 * The buffer grows to meet a long record up to this ceiling; a record that
 * still does not fit is truncated visibly rather than dropped.  Two mebibytes
 * matches the backend baseline's own default.
 */
#define PRIV_CONFIG_DEFAULT_UFLOGGER_BUFFER_MAX_BYTES  2097152U

/*!
 * Maximum length of a format pattern, including its terminator.
 *
 * A longer pattern is refused when the configuration is validated, not
 * silently shortened.
 */
#define PRIV_CONFIG_DEFAULT_UFLOGGER_PATTERN_MAX  4096U

/*!
 * Maximum length of a category name, including its terminator.
 *
 * A longer name is refused at create time, because a name that cannot be
 * represented must not be accepted and then altered.
 */
#define PRIV_CONFIG_DEFAULT_UFLOGGER_CATEGORY_MAX  1024U

/*!
 * Maximum length of a destination path, including its terminator.
 */
#define PRIV_CONFIG_DEFAULT_UFLOGGER_PATH_MAX  1024U

/*!
 * Permission bits for log files the module creates.
 *
 * Octal, and deliberately not world-readable: log lines routinely carry peer
 * identifiers and message metadata.  Private because a caller choosing a
 * destination does not thereby acquire an opinion about how the file's
 * permissions interact with the rest of the deployment.
 */
#define PRIV_CONFIG_DEFAULT_UFLOGGER_FILE_PERMS  0600U

/*!
 * Maximum number of rules a compiled configuration may hold.
 *
 * Rules are walked in order on every emitted record, so this bounds the
 * per-record cost as well as the configuration size.
 */
#define PRIV_CONFIG_DEFAULT_UFLOGGER_RULE_MAX  64U

/*!
 * Length of the asynchronous writer's queue, when that mode is enabled.
 */
#define PRIV_CONFIG_DEFAULT_UFLOGGER_ASYNC_QUEUE_LENGTH  4096U

/*!
 * Maximum length of a context key and of a context value, including
 * terminators.
 */
#define PRIV_CONFIG_DEFAULT_UFLOGGER_CONTEXT_KEY_MAX    64U
#define PRIV_CONFIG_DEFAULT_UFLOGGER_CONTEXT_VALUE_MAX  256U

#endif /* UFLIB_LOGGER_LOGGER_DEFS_PRIV_H */
