/**
 * @file logger.h
 * @brief The logger's application-facing interface.
 *
 * This is the whole of what a consumer uses.  A caller creates a logger from a
 * configuration, emits records through it, and destroys it.  At no point does
 * it name a backend, hold a backend handle, link a backend library, or learn
 * which one it was given.
 *
 * ## Using it
 *
 * @code{.c}
 * UfLogger *log_ptr = NULL;
 *
 * if (UfLoggerCreateWithDefaults(&log_ptr) != UF_LOGGER_STATUS_OK) {
 *     // No logger.  Decide here whether that is fatal; it usually is not.
 * }
 *
 * UF_LOGGER_INFO(log_ptr, "listener bound to port %u", port);
 * UF_LOGGER_WARN(log_ptr, "queue depth %zu exceeds %zu", depth, budget);
 *
 * UfLoggerDestroy(log_ptr);
 * @endcode
 *
 * ## The macros are the interface, the functions are the mechanism
 *
 * The severity macros capture @c __FILE__, @c __LINE__ and @c __func__ at the
 * call site.  Calling the functions directly records the logger's own source
 * location instead, which is why the macros exist; the functions are declared
 * because a wrapper or a @c va_list-carrying shim needs them, not because a
 * caller should reach for them first.
 *
 * Every emit entry point carries a @c printf format attribute, so a mismatched
 * argument is a compile-time diagnostic rather than a corrupted record.
 *
 * ## Threading
 *
 * A logger may be shared across threads.  Emitting, reading the level and
 * attaching context are safe concurrently.  @ref UfLoggerDestroy,
 * @ref UfLoggerReload and @ref UfLoggerSetLevel are not: each changes state
 * concurrent emitters read, so a caller must quiesce them first.  That is a
 * contract rather than a lock, because serialising every emit to make
 * reconfiguration safe would cost the hot path far more than the
 * reconfiguration is worth.
 *
 * ## Lifetime
 *
 * @ref UfLoggerDestroy releases the handle.  **Any use of a pointer to it
 * afterwards is undefined** — emitting, reading the level, attaching context,
 * or destroying it a second time.
 *
 * This is stated plainly because there is no status code for it, and the
 * absence is deliberate rather than an oversight.  The handle's memory is freed
 * by the destroy, so a function called afterwards would have to read freed
 * memory in order to work out that it should refuse; a code like
 * @c ERR_STATE_ALREADY_DESTROYED could therefore never be returned honestly,
 * and advertising one would suggest a check the library cannot perform.
 *
 * The obligation is the ordinary one for a C object handed out by pointer:
 * a caller must ensure no thread still holds the handle before destroying it.
 * A logger destroyed early does not disturb its siblings — releasing the last
 * one is what releases the process-global state — so a component can destroy
 * its own logger at shutdown without coordinating with anything else.
 *
 * ## Failure
 *
 * A logger that cannot be created is an error, but it is not a reason to abort
 * a server — which is why creation returns a status rather than exiting.
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

#ifndef UFLIB_LOGGER_LOGGER_H
#define UFLIB_LOGGER_LOGGER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>
#include <uflib/logger/logger_type.h>
#include <uflib/logger/logger_defs.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/**
 * @brief The module's default configuration, as a shared static.
 *
 * Fully populated, and a working configuration in its own right: it logs at
 * INFO and above to syslog as @c LOG_USER, with the pattern in
 * @ref logger_defs.h.  A caller can read it to learn what "sensible" means
 * here, pass it straight to @ref UfLoggerCreate, or copy it and deviate.
 *
 * The pointer refers to static storage that is never freed and never mutated.
 * A caller that wants to change a field copies the struct first; because every
 * string it references is equally static, a shallow copy is complete.
 *
 * @return The shared default configuration.  Never NULL.
 *
 * @code{.c}
 * UfLoggerConfig config = *UfLoggerProvideSaneDefaults();
 * config.category = "orders";
 * config.minimum_level = UF_LOGGER_LEVEL_DEBUG;
 *
 * UfLogger *log_ptr = NULL;
 * UfLoggerCreate(&config, &log_ptr);
 * @endcode
 */
PUBLIC_API const UfLoggerConfig *
UfLoggerProvideSaneDefaults(void);

/**
 * @brief Create a logger from a configuration.
 *
 * The configuration is validated before anything is opened.  A configuration
 * that asks for something this build cannot honour is refused with
 * @ref UF_LOGGER_STATUS_ERR_CAPABILITY rather than accepted and quietly
 * ignored — a logger that silently drops half of what it was told to do is
 * worse than one that declines.
 *
 * This is also where the process-global logging state is acquired.  The first
 * logger created takes it and the last one destroyed releases it, so a caller
 * creates loggers in any order and gets a coherent whole.  If another part of
 * the process already holds that state, creation fails with
 * @ref UF_LOGGER_STATUS_ERR_BUSY; the module does not take it by force, because
 * that would silently discard the other holder's configuration.
 *
 * On failure @p out_logger_ptr is set to NULL.  Strings referenced by
 * @p config_ptr are borrowed and must outlive the logger.
 *
 * @param config_ptr      Configuration to apply.  NULL is not accepted.
 * @param out_logger_ptr  Receives the new logger on success, NULL on failure.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 *
 * @code{.c}
 * UfLogger *log_ptr = NULL;
 * UfLoggerStatus status = UfLoggerCreate(UfLoggerProvideSaneDefaults(), &log_ptr);
 *
 * if (status != UF_LOGGER_STATUS_OK) {
 *     fprintf(stderr, "logger unavailable: %d\n", (int)status);
 * }
 * @endcode
 */
PUBLIC_API UfLoggerStatus
UfLoggerCreate(const UfLoggerConfig *config_ptr, UfLogger **out_logger_ptr);

/**
 * @brief Create a logger with the module's default configuration.
 *
 * Exactly @c UfLoggerCreate(UfLoggerProvideSaneDefaults(), out_logger_ptr).
 * Provided because the default case is the common case, and because it spares
 * a caller with no opinion from having to form one.
 *
 * @param out_logger_ptr  Receives the new logger on success, NULL on failure.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 *
 * @code{.c}
 * UfLogger *log_ptr = NULL;
 * UfLoggerCreateWithDefaults(&log_ptr);
 * @endcode
 */
PUBLIC_API UfLoggerStatus
UfLoggerCreateWithDefaults(UfLogger **out_logger_ptr);

/**
 * @brief Destroy a logger, releasing the process-global state if it held the last.
 *
 * Must not be called concurrently with any other call on the same logger, and
 * the pointer is invalid afterwards — see the lifetime note at the top of this
 * file.  A second call on the same pointer is not a safe no-op and is not
 * reported as one; the handle is gone.
 *
 * A logger destroyed early does not disturb its siblings.
 *
 * Passing NULL is legal and does nothing.
 *
 * @param logger_ptr  Logger to destroy; may be NULL.
 */
PUBLIC_API void
UfLoggerDestroy(UfLogger *logger_ptr);

/* ── Emit ───────────────────────────────────────────────────────────────── */

/**
 * @brief Emit a record at an explicit severity, from a @c va_list.
 *
 * The @c va_list entry point.  The source location is explicit because a
 * @c va_list cannot be forwarded through a variadic macro.
 *
 * @param logger_ptr    Logger to emit through.
 * @param level         Severity; must be a real level, not @ref UF_LOGGER_LEVEL_DEFAULT.
 * @param file_ptr      Source file, as @c __FILE__ would give it.
 * @param line          Source line.
 * @param function_ptr  Source function.
 * @param format_ptr    @c printf-style format, applied to @p args.
 * @param args          Arguments for @p format_ptr.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 */
PUBLIC_API UfLoggerStatus
UfLoggerVLog(UfLogger *logger_ptr, UfLoggerLevel level, const char *file_ptr, int line,
             const char *function_ptr, const char *format_ptr, va_list args);

/**
 * @brief Emit a record at an explicit severity.
 *
 * Prefer the severity macros, which supply the caller's source location.
 *
 * @param logger_ptr    Logger to emit through.
 * @param level         Severity; must be a real level, not @ref UF_LOGGER_LEVEL_DEFAULT.
 * @param file_ptr      Source file, as @c __FILE__ would give it.
 * @param line          Source line.
 * @param function_ptr  Source function.
 * @param format_ptr    @c printf-style format.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 *
 * @code{.c}
 * UfLoggerLog(log_ptr, UF_LOGGER_LEVEL_WARN, __FILE__, __LINE__, __func__,
 *             "peer %s unreachable", peer);
 * @endcode
 */
PUBLIC_API UfLoggerStatus
UfLoggerLog(UfLogger *logger_ptr, UfLoggerLevel level, const char *file_ptr, int line,
            const char *function_ptr, const char *format_ptr, ...)
    __attribute__((format(printf, 6, 7)));

/**
 * @brief Emit a record at TRACE, from the caller's own source location.
 *
 * @param logger_ptr    Logger to emit through.
 * @param file_ptr      Source file.
 * @param line          Source line.
 * @param function_ptr  Source function.
 * @param format_ptr    @c printf-style format.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 */
PUBLIC_API UfLoggerStatus
UfLoggerTrace(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
              const char *format_ptr, ...) __attribute__((format(printf, 5, 6)));

/** @brief Emit a record at DEBUG.  See @ref UfLoggerTrace for the parameter contract. */
PUBLIC_API UfLoggerStatus
UfLoggerDebug(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
              const char *format_ptr, ...) __attribute__((format(printf, 5, 6)));

/** @brief Emit a record at INFO.  See @ref UfLoggerTrace for the parameter contract. */
PUBLIC_API UfLoggerStatus
UfLoggerInfo(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
             const char *format_ptr, ...) __attribute__((format(printf, 5, 6)));

/** @brief Emit a record at NOTICE.  See @ref UfLoggerTrace for the parameter contract. */
PUBLIC_API UfLoggerStatus
UfLoggerNotice(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
               const char *format_ptr, ...) __attribute__((format(printf, 5, 6)));

/** @brief Emit a record at WARN.  See @ref UfLoggerTrace for the parameter contract. */
PUBLIC_API UfLoggerStatus
UfLoggerWarn(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
             const char *format_ptr, ...) __attribute__((format(printf, 5, 6)));

/** @brief Emit a record at ERROR.  See @ref UfLoggerTrace for the parameter contract. */
PUBLIC_API UfLoggerStatus
UfLoggerError(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
              const char *format_ptr, ...) __attribute__((format(printf, 5, 6)));

/** @brief Emit a record at CRITICAL.  See @ref UfLoggerTrace for the parameter contract. */
PUBLIC_API UfLoggerStatus
UfLoggerCritical(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
                 const char *format_ptr, ...) __attribute__((format(printf, 5, 6)));

/** @brief Emit a record at FATAL.  See @ref UfLoggerTrace for the parameter contract. */
PUBLIC_API UfLoggerStatus
UfLoggerFatal(UfLogger *logger_ptr, const char *file_ptr, int line, const char *function_ptr,
              const char *format_ptr, ...) __attribute__((format(printf, 5, 6)));

/*!
 * Emit at a named severity, from the caller's source location.
 *
 * The format argument is mandatory.  A call with no format is almost always a
 * mistake, and requiring one avoids the empty-variadic-argument extension that
 * forcing the issue would otherwise need.
 */
#define UF_LOGGER_TRACE(logger_ptr, ...) \
  UfLoggerTrace((logger_ptr), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define UF_LOGGER_DEBUG(logger_ptr, ...) \
  UfLoggerDebug((logger_ptr), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define UF_LOGGER_INFO(logger_ptr, ...) \
  UfLoggerInfo((logger_ptr), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define UF_LOGGER_NOTICE(logger_ptr, ...) \
  UfLoggerNotice((logger_ptr), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define UF_LOGGER_WARN(logger_ptr, ...) \
  UfLoggerWarn((logger_ptr), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define UF_LOGGER_ERROR(logger_ptr, ...) \
  UfLoggerError((logger_ptr), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define UF_LOGGER_CRITICAL(logger_ptr, ...) \
  UfLoggerCritical((logger_ptr), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define UF_LOGGER_FATAL(logger_ptr, ...) \
  UfLoggerFatal((logger_ptr), __FILE__, __LINE__, __func__, __VA_ARGS__)

/* ── Severity control ───────────────────────────────────────────────────── */

/**
 * @brief Change the severity floor.
 *
 * Records below the new floor are suppressed from that point on.  Test
 * @ref UF_LOGGER_CAP_RUNTIME_LEVEL first.  Not safe to call concurrently with
 * emitters on the same logger — see the threading note at the top of this file.
 *
 * @param logger_ptr  Logger to change.
 * @param level       New floor; must be a real level, not @ref UF_LOGGER_LEVEL_DEFAULT.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 *
 * @code{.c}
 * UfLoggerSetLevel(log_ptr, UF_LOGGER_LEVEL_DEBUG);
 * @endcode
 */
PUBLIC_API UfLoggerStatus
UfLoggerSetLevel(UfLogger *logger_ptr, UfLoggerLevel level);

/**
 * @brief Read the current severity floor.
 *
 * @param logger_ptr  Logger to read.
 * @return The floor, or @ref UF_LOGGER_LEVEL_FATAL for a NULL logger — the
 *         quietest answer, so a caller that lost its handle does not suddenly
 *         emit everything.
 */
PUBLIC_API UfLoggerLevel
UfLoggerGetLevel(const UfLogger *logger_ptr);

/**
 * @brief Ask whether a severity would be emitted.
 *
 * For guarding argument evaluation that is expensive to compute and cheap to
 * skip.  The answer is advisory: a record enabled here may still be suppressed
 * by the configuration, which can be more selective than the floor alone.
 *
 * @param logger_ptr  Logger to ask.
 * @param level       Severity to test.
 * @return @c true when the severity is at or above the floor and the logger is
 *         usable; @c false otherwise.
 *
 * @code{.c}
 * if (UfLoggerIsLevelEnabled(log_ptr, UF_LOGGER_LEVEL_DEBUG)) {
 *     UF_LOGGER_DEBUG(log_ptr, "state=%s", DescribeExpensively());
 * }
 * @endcode
 */
PUBLIC_API bool
UfLoggerIsLevelEnabled(const UfLogger *logger_ptr, UfLoggerLevel level);

/* ── Output control ─────────────────────────────────────────────────────── */

/**
 * @brief Force buffered records out to their destination.
 *
 * Test @ref UF_LOGGER_CAP_FLUSH first.  A build that does not declare it
 * returns @ref UF_LOGGER_STATUS_ERR_UNSUPPORTED here rather than reporting a
 * success it did not achieve.
 *
 * @param logger_ptr  Logger to flush.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 */
PUBLIC_API UfLoggerStatus
UfLoggerFlush(UfLogger *logger_ptr);

/**
 * @brief Request an archive of the current output.
 *
 * Test @ref UF_LOGGER_CAP_MANUAL_ARCHIVE first — an archive is an operation a
 * build either supports or does not, and asking a build that does not returns
 * @ref UF_LOGGER_STATUS_ERR_UNSUPPORTED rather than appearing to succeed.
 *
 * Rotation that happens on its own, at the size configured in
 * @ref UfLoggerConfig.max_file_bytes, needs no call here.
 *
 * @param logger_ptr  Logger to archive.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 */
PUBLIC_API UfLoggerStatus
UfLoggerArchive(UfLogger *logger_ptr);

/**
 * @brief Replace the logger's configuration at runtime.
 *
 * Test @ref UF_LOGGER_CAP_RELOAD first.  Not safe to call concurrently with
 * emitters.
 *
 * A caller should assume any severity change made through
 * @ref UfLoggerSetLevel is discarded: a reload recomputes routing from the new
 * configuration, and a floor that survived it would be a setting nothing in the
 * configuration accounts for.
 *
 * @param logger_ptr  Logger to reconfigure.
 * @param config_ptr  New configuration; may be NULL to re-apply the current one.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 */
PUBLIC_API UfLoggerStatus
UfLoggerReload(UfLogger *logger_ptr, const UfLoggerConfig *config_ptr);

/* ── Per-thread context ─────────────────────────────────────────────────── */

/**
 * @brief Attach a key to the calling thread's context.
 *
 * Test @ref UF_LOGGER_CAP_CONTEXT first.  Context is per-thread rather than
 * per-logger, matching the way a request or a connection is handled by one
 * thread at a time: a value set once at the top of a request can then appear on
 * every record that request emits, without being threaded through the call
 * tree.
 *
 * @param logger_ptr  Logger to attach through.
 * @param key_ptr     Context key; must be non-empty.
 * @param value_ptr   Value to associate.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 *
 * @code{.c}
 * UfLoggerSetContext(log_ptr, "session", session_id);
 * UF_LOGGER_INFO(log_ptr, "authenticated");
 * UfLoggerRemoveContext(log_ptr, "session");
 * @endcode
 */
PUBLIC_API UfLoggerStatus
UfLoggerSetContext(UfLogger *logger_ptr, const char *key_ptr, const char *value_ptr);

/**
 * @brief Remove one key from the calling thread's context.
 *
 * Removing a key that was never set is not an error.
 *
 * @param logger_ptr  Logger to detach through.
 * @param key_ptr     Context key to remove.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 */
PUBLIC_API UfLoggerStatus
UfLoggerRemoveContext(UfLogger *logger_ptr, const char *key_ptr);

/**
 * @brief Remove every key from the calling thread's context.
 *
 * The counterpart to setting several: a worker returning to a pool clears once
 * rather than enumerating what it set.
 *
 * @param logger_ptr  Logger to clear through.
 * @return @ref UF_LOGGER_STATUS_OK, or a specific error.
 */
PUBLIC_API UfLoggerStatus
UfLoggerClearContext(UfLogger *logger_ptr);

/* ── Introspection ──────────────────────────────────────────────────────── */

/**
 * @brief What this build can honour, as @c UF_LOGGER_CAP_* bits.
 *
 * Read the mask before calling @ref UfLoggerFlush, @ref UfLoggerArchive,
 * @ref UfLoggerReload, @ref UfLoggerSetLevel or @ref UfLoggerSetContext, so that
 * an operation this build does not support is a decision rather than a
 * surprise.  A mask of 0 means the build declares nothing, which is legal and
 * means exactly that.
 *
 * @param logger_ptr  Logger to ask.
 * @return The capability bitmask, or 0 for a NULL logger.
 *
 * @code{.c}
 * if (UfLoggerGetCapabilities(log_ptr) & UF_LOGGER_CAP_RELOAD) {
 *     UfLoggerReload(log_ptr, &new_config);
 * }
 * @endcode
 */
PUBLIC_API uint32_t
UfLoggerGetCapabilities(const UfLogger *logger_ptr);

/**
 * @brief Describe the logger's effective configuration as a JSON string.
 *
 * Reports what the logger is doing rather than what it was asked for: defaults
 * are resolved, and the capability mask is included, so a reader sees both the
 * intent and what this build will honour.  The gap between those two is where
 * an unexpected behaviour lives, which is what makes this worth reading when a
 * configuration is not doing what it appears to say.
 *
 * If @p provided_ptr is NULL a fresh descriptor is allocated, which the caller
 * releases with @c BufferDescriptorRelease() and then @c free().  Otherwise the
 * caller's descriptor is appended to, which is what lets a caller compose
 * several descriptions into one document.
 *
 * @param logger_ptr    Logger to describe; NULL emits an error object.
 * @param provided_ptr  Optional caller-owned descriptor; NULL to allocate one.
 * @return The populated descriptor, or NULL only if a fresh one could not be
 *         allocated.
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 512);
 * UfLoggerDescribeConfiguration(log_ptr, &bd);
 * printf("%s\n", bd.data);
 * BufferDescriptorRelease(&bd);
 * @endcode
 */
PUBLIC_API BufferDescriptor *
UfLoggerDescribeConfiguration(const UfLogger *logger_ptr, BufferDescriptor *provided_ptr);

#ifdef __cplusplus
}
#endif

#endif /* UFLIB_LOGGER_LOGGER_H */
