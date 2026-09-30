/**
 * @file ufcommand_host_app.c
 * @brief A minimal host application integrating ufcommand.
 *
 * Demonstrates, in the order a real host does them: create a logger, create a
 * registry configured to use it, register commands, add user-owned aliases and a
 * key binding, layer a user configuration file over the built-ins, dispatch typed
 * lines and a key chord, write the telemetry record, and persist user state.
 *
 * Build (standalone, against an installed uflib):
 *   cc -std=gnu17 -o ufcommand_host_app ufcommand_host_app.c -luflib
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uflib/ufcommand/ufcommand.h>
#include <uflib/ufcommand/ufcommand_completion.h>
#include <uflib/ufcommand/ufcommand_telemetry.h>
#include <uflib/logger/logger.h>

#define CONFIG_PATH   "ufcommand_host_app.conf.lua"
#define TELEMETRY_PATH "ufcommand_host_app.telemetry.json"
#define SAVED_PATH    "ufcommand_host_app.saved.lua"

/* ── Handlers ─────────────────────────────────────────────────────────────── */

static UfCommandStatus
OnStatus(UfCommandContext *context, const char *name, const UfCommandArg *args, size_t arg_count,
         void *user_data)
{
    /* user_data is whatever the host registered: borrowed, never freed here. */
    printf("    [%s] session=%s\n", name, (const char *)user_data);
    (void)context; (void)args; (void)arg_count;
    return UFCOMMAND_OK;
}

static UfCommandStatus
OnRemoteAdd(UfCommandContext *context, const char *name, const UfCommandArg *args, size_t arg_count,
            void *user_data)
{
    /* args[i].length is the DECODED length; value is not NUL-terminated beyond it. */
    printf("    [%s] remote '%.*s' -> %.*s\n", name, (int)args[0].length, args[0].value,
           (int)args[1].length, args[1].value);
    (void)context; (void)user_data; (void)arg_count;
    return UFCOMMAND_OK;
}

static UfCommandStatus
OnRebuild(UfCommandContext *context, const char *name, const UfCommandArg *args, size_t arg_count,
          void *user_data)
{
    (void)context; (void)name; (void)args; (void)arg_count; (void)user_data;
    /* A handler may report its own failure; it lands in handler_status, not
     * dispatch_status, so the host can tell "did not match" from "ran and failed". */
    fprintf(stderr, "    rebuild: no toolchain configured\n");
    return UFCOMMAND_HANDLER_ERROR;
}

/* ── Dispatch helper ──────────────────────────────────────────────────────── */

static void
RunLine(UfCommandParser *parser, UfCommandRegistry *registry, UfCommandResult *result, const char *line)
{
    UfCommandStatus status = UfCommandParserExecute(parser, registry, NULL, line, result);

    printf("  %-34s -> %-16s", line, UfCommandStatusName(status));
    if (UfCommandResultGetResolvedCommand(result) != NULL) {
        printf(" '%s', %zu arg(s)", UfCommandResultGetResolvedCommand(result),
               UfCommandResultGetArgCount(result));
    }
    if (status != UFCOMMAND_OK && UfCommandResultGetDispatchStatus(result) == UFCOMMAND_OK) {
        /* dispatch succeeded, the handler refused */
        printf(" (handler: %s)", UfCommandStatusName(UfCommandResultGetHandlerStatus(result)));
    }
    printf("\n");
}

/* ── Completion helper ────────────────────────────────────────────────────── */

static void
Suggest(UfCommandCompletion *completion, const char *line, size_t cursor)
{
    UfCommandCompletionResult     *result = NULL;
    UfCommandCompletionResultInfo  info   = { 0, 0, 0 };
    UfCommandStatus status = UfCommandCompletionSuggest(completion, line, cursor, &result);

    printf("  %-20s -> %-8s", line, UfCommandStatusName(status));
    if (status == UFCOMMAND_OK && UfCommandCompletionResultGetInfo(result, &info) == UFCOMMAND_OK) {
        /* The range spans the whole active token -- what a line editor replaces. */
        printf(" replace [%zu,%zu)", info.replacement_start, info.replacement_end);
        for (size_t i = 0; i < info.count; ++i) {
            printf(" %s", UfCommandCompletionResultGetCandidate(result, i)->text);
        }
    }
    printf("\n");
    UfCommandCompletionResultDestroy(result);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int
main(void)
{
    UfLogger          *logger   = NULL;
    UfCommandRegistry *registry = NULL;
    UfCommandParser   *parser   = NULL;
    UfCommandResult   *result   = NULL;

    /* 1. The logger first, because the registry borrows it and must not outlive it. */
    if (UfLoggerCreateWithDefaults(&logger) != UF_LOGGER_STATUS_OK) {
        fprintf(stderr, "logger: creation failed\n");
        return EXIT_FAILURE;
    }

    /* 2. A registry that reports through the logger, records usage, and can be
     *    asked for completions. */
    UfCommandRegistryConfig config = { 0 };
    config.uf_logger          = logger;  /* optional; NULL logs nothing */
    config.telemetry_enabled  = true;    /* optional; off by default */
    config.completion_enabled = true;    /* optional; off by default */

    if (UfCommandRegistryCreate(&config, &registry) != UFCOMMAND_OK ||
        UfCommandParserCreate(NULL, &parser) != UFCOMMAND_OK ||
        UfCommandResultCreate(&result) != UFCOMMAND_OK) {
        fprintf(stderr, "setup failed\n");
        return EXIT_FAILURE;
    }

    /* 3. Host-owned commands.  Names may be multi-token, any depth. */
    UfCommandDefinition commands[] = {
        { "status",     "show state",     OnStatus,   "sess-42", 0, 0 },
        { "remote add", "add a remote",   OnRemoteAdd, NULL,     2, 2 },
        { "rebuild",    "rebuild (fails)", OnRebuild,  NULL,     0, 0 },
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        UfCommandRegistryAdd(registry, &commands[i]);
    }

    /* 4. User-owned state: an alias and a chord. */
    UfCommandAliasDefinition alias = { "ra", "remote add $1 $2" };
    UfCommandRegistryAddAlias(registry, &alias);

    UfCommandKeyConfig keys = { UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT };
    UfCommandRegistrySetKeyConfig(registry, &keys);

    UfCommandBinding binding = { UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT, "P", "status" };
    UfCommandRegistryAddBinding(registry, &binding);

    /* 5. Layer a user configuration over the built-ins.  Register commands FIRST:
     *    the file's aliases are checked against the command namespace, and a
     *    collision is refused rather than silently displacing a built-in. */
    FILE *seed = fopen(CONFIG_PATH, "wb");
    if (seed != NULL) {
        fputs("return {\n  version = 1,\n  aliases = { [\"st\"] = \"status\" },\n}\n", seed);
        fclose(seed);
    }
    printf("load user configuration: %s\n",
           UfCommandStatusName(UfCommandRegistryLoadUserConfig(registry, CONFIG_PATH)));

    /* 6. Dispatch typed lines. */
    printf("\ndispatch:\n");
    RunLine(parser, registry, result, "status");
    RunLine(parser, registry, result, "ra origin git@example.com");
    RunLine(parser, registry, result, "st");
    RunLine(parser, registry, result, "status too many");
    RunLine(parser, registry, result, "rebuild");
    RunLine(parser, registry, result, "nosuch");

    /* 7. Ask for completions.  The engine reads the registry live rather than
     *    snapshotting it, so the alias loaded in step 5 above is completable
     *    even though it arrived after the engine did. */
    printf("\nsuggest:\n");
    UfCommandCompletion *completion = UfCommandRegistryGetCompletion(registry);
    Suggest(completion, "re", 2);            /* a command component */
    Suggest(completion, "st", 2);            /* the loaded alias, and "status" */
    Suggest(completion, "remote ", 7);       /* the next component of a nested name */
    Suggest(completion, "remote add", 8);    /* cursor mid-token: range covers "add" */

    /* 8. Dispatch a key chord against the configured modifiers. */
    printf("\nkeyboard:\n");
    UfCommandStatus chord = UfCommandParserExecuteDefaultBinding(parser, registry, NULL, "P", result);
    printf("  CTRL+SHIFT+P                       -> %s\n", UfCommandStatusName(chord));
    chord = UfCommandParserExecuteBinding(parser, registry, NULL, UFCOMMAND_MOD_ALT, "P", result);
    printf("  ALT+P                              -> %s\n", UfCommandStatusName(chord));

    /* 9. Persist user state.  Only aliases, bindings and key config are written;
     *    commands are not, because their handlers belong to the host. */
    printf("\nsave user configuration: %s\n",
           UfCommandStatusName(UfCommandRegistrySaveUserConfig(registry, SAVED_PATH)));

    /* 10. Write the usage record.  Deterministic and sorted, so it diffs. */
    UfCommandTelemetry *telemetry = UfCommandRegistryGetTelemetry(registry);
    printf("write telemetry:         %s\n",
           UfCommandStatusName(UfCommandTelemetrySaveJson(telemetry, TELEMETRY_PATH)));

    /* 11. Teardown: the registry first, then the logger it borrowed. */
    UfCommandResultDestroy(result);
    UfCommandParserDestroy(parser);
    UfCommandRegistryDestroy(registry);
    UfLoggerDestroy(logger);
    return EXIT_SUCCESS;
}
