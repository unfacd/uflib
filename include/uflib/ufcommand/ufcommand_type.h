/**
 * @file ufcommand_type.h
 * @brief UfCommand public types — status codes, option structures, definitions.
 *
 * The opaque handle types (`UfCommandRegistry`, `UfCommandParser`,
 * `UfCommandContext`, `UfCommandResult`) are declared here and defined only in
 * the library's private headers, so a consumer cannot depend on their layout.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_UFCOMMAND_UFCOMMAND_TYPE_H
#define UFLIB_UFCOMMAND_UFCOMMAND_TYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>
#include <uflib/logger/logger.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UfCommandContext  UfCommandContext;
typedef struct UfCommandRegistry UfCommandRegistry;
typedef struct UfCommandParser   UfCommandParser;
typedef struct UfCommandResult   UfCommandResult;

/*!
 * @brief Outcome of an API call, a dispatch, or a handler invocation.
 *
 * The failures a caller has to distinguish are deliberately separate rather
 * than folded into one code: @c UFCOMMAND_ARGUMENT_COUNT is the user typing the
 * wrong number of arguments, @c UFCOMMAND_INVALID_ARGUMENT is the host passing
 * a bad pointer, and @c UFCOMMAND_NO_MEMORY is the allocator failing.
 */
typedef enum UfCommandStatus
{
  UFCOMMAND_OK = 0,               ///< success
  UFCOMMAND_NOT_FOUND,            ///< no such command, or nothing to remove
  UFCOMMAND_PARSE_ERROR,          ///< the input line could not be tokenised
  UFCOMMAND_INVALID_ARGUMENT,     ///< a caller-supplied pointer or value is invalid
  UFCOMMAND_ARGUMENT_COUNT,       ///< the input satisfied no registered argument contract
  UFCOMMAND_DUPLICATE,            ///< an entry of that name already exists
  UFCOMMAND_NO_MEMORY,            ///< allocation failed
  UFCOMMAND_BUFFER_TOO_SMALL,     ///< a caller-supplied buffer was too small
  UFCOMMAND_HANDLER_ERROR,        ///< the command handler reported failure
  UFCOMMAND_ALIAS_CYCLE,          ///< an alias chain refers back to itself
  UFCOMMAND_ALIAS_DEPTH_EXCEEDED, ///< an alias chain is longer than the configured bound
  UFCOMMAND_CONFLICT,             ///< the name is already taken in the shared namespace
  UFCOMMAND_IO_ERROR,             ///< a file could not be read or written
  UFCOMMAND_CONFIG_ERROR,         ///< the user configuration is malformed
  UFCOMMAND_UNSUPPORTED_VERSION,  ///< the configuration declares an unknown version
  UFCOMMAND_INVALID_COMMAND,      ///< the command definition is unusable as given
  UFCOMMAND_INVALID_ALIAS,        ///< the alias definition is unusable as given
  UFCOMMAND_NO_BINDING,           ///< no keyboard binding matches that chord
  UFCOMMAND_CANCELLED             ///< the operation was abandoned
} UfCommandStatus;

/*!
 * @brief Keyboard modifier bits, combinable into a mask.
 *
 * In C++ the bitwise OR of two enumerators has type @c int, so a C++ host must
 * cast when combining these; in C the conversion is implicit.
 */
typedef enum UfCommandModifier
{
  UFCOMMAND_MOD_NONE  = 0u,      ///< no modifier
  UFCOMMAND_MOD_CTRL  = 1u << 0, ///< Control
  UFCOMMAND_MOD_SHIFT = 1u << 1, ///< Shift
  UFCOMMAND_MOD_ALT   = 1u << 2, ///< Alt
  UFCOMMAND_MOD_META  = 1u << 3  ///< Meta / Super
} UfCommandModifier;

/*!
 * @brief One argument handed to a command handler.
 *
 * @c value points into storage owned by the result object and remains valid
 * until that result is reset or destroyed.  @c length is the decoded length, so
 * a @c value containing an escape is measured after unescaping, and a @c value
 * may not be assumed NUL-terminated beyond @c length.
 */
typedef struct UfCommandArg
{
  const char *value;  ///< borrowed pointer, valid for the result's lifetime
  size_t      length; ///< decoded length of @c value, in bytes
} UfCommandArg;

/*!
 * @brief A command implementation.
 *
 * @param context       the context supplied to the parser call, or NULL
 * @param command_name  the resolved command name, borrowed
 * @param args          the argument slice; NULL when @c arg_count is zero
 * @param arg_count     number of arguments
 * @param user_data     the pointer registered with the command
 *
 * @return UFCOMMAND_OK on success, or any status the host wishes to report.
 *         The value is recorded as the result's handler status.
 */
typedef UfCommandStatus (*UfCommandHandlerCallback)(UfCommandContext *  context, const char *command_name,
                                                    const UfCommandArg *args, size_t arg_count, void *user_data);

/*!
 * @brief A command registration.
 *
 * @c name may contain space-separated components ("remote add"); it is
 * canonicalised on registration and may be arbitrary in depth.  @c min_args and
 * @c max_args constrain the argument count, with @c SIZE_MAX meaning unbounded.
 */
typedef struct UfCommandDefinition
{
  const char *             name;        ///< command name, canonicalised on Add
  const char *             description; ///< optional; NULL is stored as empty
  UfCommandHandlerCallback handler;     ///< must not be NULL
  void *                   user_data;   ///< borrowed; never freed by the library
  size_t                   min_args;    ///< inclusive lower bound
  size_t                   max_args;    ///< inclusive upper bound, or SIZE_MAX
} UfCommandDefinition;

/*!
 * @brief A user alias and the text it expands to.
 *
 * @c expansion is tokenised with the parser's own rules and supports @c $1 to
 * @c $9 and @c $*; any other @c $ form is a literal token.  The alias name may
 * not contain a separator.
 */
typedef struct UfCommandAliasDefinition
{
  const char *alias;     ///< single token, no whitespace
  const char *expansion; ///< replacement text, may be multi-token
} UfCommandAliasDefinition;

/*!
 * @brief A keyboard chord bound to a command or alias name.
 *
 * @c command is executed through the same path as a typed line, so it may name
 * an alias; it may carry arguments ("co main").
 */
typedef struct UfCommandBinding
{
  UfCommandModifier modifiers; ///< modifier mask
  const char *      key;       ///< key name, e.g. "P"
  const char *      command;   ///< command or alias text to execute
} UfCommandBinding;

/*!
 * @brief Tokeniser limits and alias bound.
 *
 * All three size fields must be non-zero: a parser that cannot tokenise, or
 * that can never expand an alias, is rejected at creation rather than failing
 * every later call.
 */
typedef struct UfCommandParserConfig
{
  size_t max_tokens;       ///< maximum tokens in one input line
  size_t max_token_length; ///< maximum decoded length of one token
  char   quote_char;       ///< quoting character
  char   escape_char;      ///< escaping character
  size_t max_alias_depth;  ///< maximum alias expansion rounds, must be non-zero
} UfCommandParserConfig;

/*!
 * @brief Registry sizing and namespace policy.
 *
 * @c max_command_components caps how many space-separated components a command
 * name may have; zero or @c SIZE_MAX leaves it unbounded, which matches a
 * dispatcher that places no cap on the prefix it will resolve.
 */
typedef struct UfCommandRegistryConfig
{
  size_t    initial_capacity;       ///< hint; rounded up to a power of two, minimum 16
  size_t    max_command_components; ///< 0 or SIZE_MAX for unbounded
  /*!
   * Record command, alias and binding usage so that
   * @c UfCommandRegistryDumpTelemetryJson() has something to write.  Off by
   * default: counting costs roughly a quarter of a multi-prefix dispatch, and
   * a host that never reads the record should not pay for it.
   */
  bool      telemetry_enabled;
  /*!
   * Create a predictive-completion engine that lives as long as the registry.
   * Off by default: a host that never asks for a suggestion should not carry
   * the engine, nor the lookup it does per call.
   *
   * Declared here -- immediately after @c telemetry_enabled, and ahead of the
   * pointer below -- so the struct keeps its published 32-byte size.  Appending
   * it instead would make the library read one byte past the end of a consumer
   * compiled against an older header, where an arbitrary value could switch the
   * engine on for a host that never asked.
   */
  bool      completion_enabled;
  /*!
   * Optional.  NULL logs nothing, which is the default.
   *
   * Borrowed, not owned: the registry never destroys it, and it must outlive
   * every registry that references it -- so create the logger first and destroy
   * it last.  Failures the module would otherwise report only through a status
   * code are logged, along with registry creation and user-configuration
   * load/save; see the design document for the level policy.
   */
  UfLogger *uf_logger;
} UfCommandRegistryConfig;

/*!
 * @brief The modifier set used when a binding is dispatched by key alone.
 */
typedef struct UfCommandKeyConfig
{
  UfCommandModifier default_modifiers; ///< defaults to CTRL|SHIFT
} UfCommandKeyConfig;

/*!
 * @brief Optional host data carried by a context.
 */
typedef struct UfCommandContextConfig
{
  void *user_data; ///< borrowed; never freed by the library
} UfCommandContextConfig;

/*!
 * @brief The two independent statuses carried by a result.
 *
 * @c dispatch_status reports whether the input was matched and executed;
 * @c handler_status reports what the handler returned.  They are separate so a
 * host can tell "the command did not run" from "the command ran and failed".
 */
typedef struct UfCommandResultInfo
{
  UfCommandStatus dispatch_status; ///< outcome of matching and invoking
  UfCommandStatus handler_status;  ///< what the handler returned, or UFCOMMAND_OK
} UfCommandResultInfo;

#ifdef __cplusplus
}
#endif
#endif
