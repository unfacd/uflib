/**
 * @file ufcommand.h
 * @brief UfCommand — command registry, alias expansion and keyboard dispatch.
 *
 * UfCommand is a host-application-independent command registry.  A host
 * registers commands with their handlers, optionally loads a user configuration
 * of aliases and key bindings, and then feeds input lines or key chords to the
 * parser.  The library owns no event loop, no UI and no I/O policy.
 *
 * ## Model
 *
 * Input is tokenised, then matched against the longest registered command name:
 *
 * @code
 * command [component ...] [arguments ...]
 * @endcode
 *
 * A registered name may be any depth, so `remote` and `remote add` coexist and
 * the longer match wins.  Remaining tokens become the handler's arguments.
 *
 * ## Names, aliases and the shared namespace
 *
 * Command names are canonicalised on registration: separator runs collapse to a
 * single space and surrounding whitespace is trimmed, and every entry point that
 * takes a name resolves it through the same canonical form.  Commands and
 * aliases share one namespace, so registering either over the other's name is
 * reported as @c UFCOMMAND_CONFLICT rather than silently displacing it.
 *
 * ## Error reporting
 *
 * A result carries two independent statuses -- dispatch and handler -- so a host
 * can distinguish "the input did not match" from "the command ran and failed".
 * @c UfCommandResultGetStatus() returns whichever of the two is decisive.
 *
 * ## Thread safety
 *
 * None of these objects is safe for concurrent use.  While a handler is
 * executing, mutating the registry is rejected with @c UFCOMMAND_CONFLICT
 * rather than corrupting the dispatch in progress.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_UFCOMMAND_UFCOMMAND_H
#define UFLIB_UFCOMMAND_UFCOMMAND_H

#include <stddef.h>

#include <uflib/uflib_defs.h>
#include <uflib/ufcommand/ufcommand_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a registry.
 *
 * @param config Sizing and namespace policy, or NULL for the defaults.
 * @param out_registry Receives the new registry; set to NULL on failure.
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_INVALID_ARGUMENT or @c UFCOMMAND_NO_MEMORY.
 *
 * @code{.c}
 * UfCommandRegistry *registry = NULL;
 * if (UfCommandRegistryCreate(NULL, &registry) != UFCOMMAND_OK) {
 *     return EXIT_FAILURE;
 * }
 * @endcode
 */
PUBLIC_API UfCommandStatus UfCommandRegistryCreate(const UfCommandRegistryConfig *config,
                                                   UfCommandRegistry **           out_registry);

/**
 * @brief Destroy a registry and everything it owns.  @p registry may be NULL.
 *
 * Handler @c user_data is borrowed and is never freed here.
 */
PUBLIC_API void UfCommandRegistryDestroy(UfCommandRegistry *registry);

/**
 * @brief Register a command.
 *
 * @param registry Target registry.
 * @param definition The command; @c name and @c handler must be non-NULL.
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_INVALID_ARGUMENT, @c UFCOMMAND_DUPLICATE,
 *         @c UFCOMMAND_CONFLICT, @c UFCOMMAND_INVALID_COMMAND or @c UFCOMMAND_NO_MEMORY.
 *
 * The name is canonicalised and stored; registration during an active dispatch
 * is refused with @c UFCOMMAND_CONFLICT.
 *
 * @code{.c}
 * UfCommandDefinition def = { "remote add", "Add a remote", OnRemoteAdd, ctx, 2, 2 };
 * if (UfCommandRegistryAdd(registry, &def) != UFCOMMAND_OK) {
 *     return EXIT_FAILURE;
 * }
 * @endcode
 */
PUBLIC_API UfCommandStatus UfCommandRegistryAdd(UfCommandRegistry *registry, const UfCommandDefinition *definition);

/**
 * @brief Remove a command by name.
 *
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_NOT_FOUND, @c UFCOMMAND_INVALID_ARGUMENT
 *         or @c UFCOMMAND_CONFLICT if a dispatch is in progress.
 */
PUBLIC_API UfCommandStatus UfCommandRegistryRemove(UfCommandRegistry *registry, const char *name);

/**
 * @brief Look up a command.
 *
 * @param out_definition Receives borrowed pointers into the registry: @c name
 *        and @c description are invalidated by a later @c Remove of that
 *        command.  Copy them if they must outlive it.
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_NOT_FOUND or @c UFCOMMAND_INVALID_ARGUMENT.
 */
PUBLIC_API UfCommandStatus UfCommandRegistryFind(const UfCommandRegistry *registry, const char *name,
                                                 UfCommandDefinition *    out_definition);

/**
 * @brief Register an alias.
 *
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_INVALID_ALIAS, @c UFCOMMAND_DUPLICATE or
 *         @c UFCOMMAND_CONFLICT when the name is already a command.
 *
 * @code{.c}
 * UfCommandAliasDefinition alias = { "co", "checkout $1" };
 * UfCommandRegistryAddAlias(registry, &alias);
 * @endcode
 */
PUBLIC_API UfCommandStatus UfCommandRegistryAddAlias(UfCommandRegistry *             registry,
                                                     const UfCommandAliasDefinition *definition);

/**
 * @brief Remove an alias by name.
 *
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_NOT_FOUND or @c UFCOMMAND_INVALID_ARGUMENT.
 */
PUBLIC_API UfCommandStatus UfCommandRegistryRemoveAlias(UfCommandRegistry *registry, const char *alias);

/**
 * @brief Bind a modifier-and-key chord to command text.
 *
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_INVALID_ARGUMENT, @c UFCOMMAND_DUPLICATE
 *         or @c UFCOMMAND_CONFLICT.
 */
PUBLIC_API UfCommandStatus UfCommandRegistryAddBinding(UfCommandRegistry *registry, const UfCommandBinding *binding);

/**
 * @brief Remove the binding for a chord.
 *
 * A chord with no binding is @c UFCOMMAND_NOT_FOUND: this is a registry
 * mutation, not a dispatch, so the dispatch-specific @c UFCOMMAND_NO_BINDING
 * does not apply here.
 */
PUBLIC_API UfCommandStatus UfCommandRegistryRemoveBinding(UfCommandRegistry *registry, UfCommandModifier modifiers,
                                                          const char *       key);

/**
 * @brief Set the modifier set used by @c UfCommandParserExecuteDefaultBinding().
 */
PUBLIC_API UfCommandStatus UfCommandRegistrySetKeyConfig(UfCommandRegistry *registry, const UfCommandKeyConfig *config);

/**
 * @brief Read the current key configuration.
 */
PUBLIC_API UfCommandStatus UfCommandRegistryGetKeyConfig(const UfCommandRegistry *registry,
                                                         UfCommandKeyConfig *     out_config);

/**
 * @brief Write the user-owned aliases, bindings and key configuration to @p path.
 *
 * Only user-owned state is written; registered commands are not, because their
 * handlers and @c user_data belong to the host.  Output is sorted so repeated
 * saves of the same state are byte-identical, and the write goes to a sibling
 * temporary file that is renamed into place, so a failure cannot truncate the
 * caller's existing configuration.
 *
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_INVALID_ARGUMENT or @c UFCOMMAND_IO_ERROR.
 */
PUBLIC_API UfCommandStatus UfCommandRegistrySaveUserConfig(const UfCommandRegistry *registry, const char *path);

/**
 * @brief Load aliases, bindings and key configuration from @p path.
 *
 * The file is a Lua-styled table document, parsed by the library and never
 * executed.  The whole document is validated and staged before anything is
 * applied, so a malformed or conflicting file leaves the registry untouched.
 * An existing alias or binding is never overwritten: that is
 * @c UFCOMMAND_CONFLICT.
 *
 * @return @c UFCOMMAND_OK, @c UFCOMMAND_INVALID_ARGUMENT, @c UFCOMMAND_IO_ERROR,
 *         @c UFCOMMAND_CONFIG_ERROR, @c UFCOMMAND_UNSUPPORTED_VERSION,
 *         @c UFCOMMAND_DUPLICATE or @c UFCOMMAND_CONFLICT.
 */
PUBLIC_API UfCommandStatus UfCommandRegistryLoadUserConfig(UfCommandRegistry *registry, const char *path);

/**
 * @brief Create a context to carry host data through a dispatch.
 *
 * @param config Optional initial user data, or NULL.
 * @param out_context Receives the context; set to NULL on failure.
 */
PUBLIC_API UfCommandStatus UfCommandContextCreate(const UfCommandContextConfig *config, UfCommandContext **out_context);

/** @brief Destroy a context.  @p context may be NULL. */
PUBLIC_API void UfCommandContextDestroy(UfCommandContext *context);

/** @brief Replace the context's user data.  The previous value is not freed. */
PUBLIC_API UfCommandStatus UfCommandContextSetUserData(UfCommandContext *context, void *user_data);

/** @brief Read the context's user data, or NULL. */
PUBLIC_API void *UfCommandContextGetUserData(const UfCommandContext *context);

/* ── Parser ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create a parser.
 *
 * @param config Tokeniser limits and the alias bound, or NULL for the defaults.
 *        Every size field must be non-zero; a config that cannot tokenise or
 *        could never expand an alias is rejected up front.
 * @param out_parser Receives the parser; set to NULL on failure.
 */
PUBLIC_API UfCommandStatus UfCommandParserCreate(const UfCommandParserConfig *config, UfCommandParser **out_parser);

/** @brief Destroy a parser.  @p parser may be NULL. */
PUBLIC_API void UfCommandParserDestroy(UfCommandParser *parser);

/**
 * @brief Parse and dispatch one input line.
 *
 * @param parser The parser.
 * @param registry The registry to resolve against.
 * @param context Passed to the handler, or NULL.
 * @param line The input; NULL is invalid.
 * @param out_result Reset before use and populated with the outcome.
 * @return The dispatch status, or the handler's status when the command ran.
 *
 * @code{.c}
 * UfCommandResult *result = NULL;
 * UfCommandResultCreate(&result);
 * UfCommandStatus st = UfCommandParserExecute(parser, registry, NULL,
 *                                             "remote add origin git@example.com", result);
 * if (st == UFCOMMAND_OK) {
 *     printf("dispatched %s with %zu argument(s)\n",
 *            UfCommandResultGetResolvedCommand(result),
 *            UfCommandResultGetArgCount(result));
 * }
 * UfCommandResultDestroy(result);
 * @endcode
 */
PUBLIC_API UfCommandStatus UfCommandParserExecute(UfCommandParser * parser, UfCommandRegistry *registry,
                                                  UfCommandContext *context, const char *      line,
                                                  UfCommandResult * out_result);

/**
 * @brief Dispatch the command bound to one keyboard chord.
 *
 * @return @c UFCOMMAND_NO_BINDING when no chord matches, otherwise as
 *         @c UfCommandParserExecute().
 */
PUBLIC_API UfCommandStatus UfCommandParserExecuteBinding(UfCommandParser * parser, UfCommandRegistry *registry,
                                                         UfCommandContext *context, UfCommandModifier modifiers,
                                                         const char *      key, UfCommandResult *     out_result);

/**
 * @brief Dispatch a key against the registry's configured default modifiers.
 *
 * @code{.c}
 * UfCommandResultReset(result);
 * UfCommandParserExecuteDefaultBinding(parser, registry, NULL, "P", result);
 * @endcode
 */
PUBLIC_API UfCommandStatus UfCommandParserExecuteDefaultBinding(UfCommandParser * parser, UfCommandRegistry *registry,
                                                                UfCommandContext *context, const char *      key,
                                                                UfCommandResult * out_result);

/* ── Result ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create a result.  One result may be reused across many calls: every
 *        execute entry point resets it first.
 */
PUBLIC_API UfCommandStatus UfCommandResultCreate(UfCommandResult **out_result);

/** @brief Destroy a result.  @p result may be NULL. */
PUBLIC_API void UfCommandResultDestroy(UfCommandResult *result);

/** @brief Release the contents of a result and clear its statuses. */
PUBLIC_API void UfCommandResultReset(UfCommandResult *result);

/**
 * @brief The decisive status: the dispatch status when dispatch failed,
 *        otherwise the handler status.
 */
PUBLIC_API UfCommandStatus UfCommandResultGetStatus(const UfCommandResult *result);

/** @brief The dispatch status alone: did the input match and execute? */
PUBLIC_API UfCommandStatus UfCommandResultGetDispatchStatus(const UfCommandResult *result);

/** @brief The handler status alone: what the handler returned, or UFCOMMAND_OK. */
PUBLIC_API UfCommandStatus UfCommandResultGetHandlerStatus(const UfCommandResult *result);

/** @brief Both statuses at once. */
PUBLIC_API UfCommandStatus UfCommandResultGetInfo(const UfCommandResult *result, UfCommandResultInfo *out_info);

/**
 * @brief The matched command name, or NULL when nothing was dispatched.
 *
 * The pointer is borrowed from the result and is invalidated by reset or
 * destroy -- and, because it names the registry entry, by removing that
 * command.
 */
PUBLIC_API const char *UfCommandResultGetResolvedCommand(const UfCommandResult *result);

/** @brief Number of arguments handed to the handler. */
PUBLIC_API size_t UfCommandResultGetArgCount(const UfCommandResult *result);

/**
 * @brief The argument slice, or NULL when there were no arguments.
 *
 * Borrowed from the result and invalidated by reset or destroy.
 */
PUBLIC_API const UfCommandArg *UfCommandResultGetArgs(const UfCommandResult *result);

/**
 * @brief A stable, human-readable name for a status value.
 *
 * @return A static string; "UNKNOWN" for a value outside the enum.
 */
PUBLIC_API const char *UfCommandStatusName(UfCommandStatus status);

#ifdef __cplusplus
}
#endif
#endif
