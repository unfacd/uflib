/**
 * @file ufcommand_stress.c
 * @brief Standalone adversarial stress and fuzz driver for ufcommand.
 *
 * Not registered with CTest: it takes --iterations/--seed and runs for as long
 * as it is asked to.  Build it alongside the module and run it directly.
 *
 * The library documents that the registry is not safe for concurrent mutation
 * or concurrent dispatch, so this driver is deliberately single-threaded --
 * adding threads here would be testing a contract the module does not claim,
 * and a race found that way would say nothing about the library.  What it does
 * instead is hammer the *sequential* invariants that silently wrong code
 * violates: every registered name must be findable and must dispatch to itself,
 * every removed name must be gone, and a dispatch that reports success must
 * actually have reached a handler.
 *
 * Exit status is 0 only when no invariant was violated.
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
#include <stdbool.h>
#include <stdint.h>

#include "ufcommand/ufcommand.h"

#define STRESS_MAX_KEYS 4096
#define STRESS_LINE_MAX 4096

/* Named here so both the biased and the purely random phases draw from them.
 * Some entries are multi-token, so longest-prefix resolution is exercised too. */
static const char *sDispatchVocabulary[] = {
    "status",  "build",  "quit",   "echo",         "remote",
    "remote add", "remote add deep", "config get", "config set",
};

static const char sRandomAlphabet[] = "abcXY \"\\$*0123456789\t";

typedef struct {
    long     iterations;
    long     seed;
    size_t   initial_capacity;
    size_t   key_count;
} Options;

typedef struct {
    long     handler_calls;
    long     errors;
    long     dispatched;
    long     not_found;
    long     parse_error;
    long     invalid_argument;
    long     argument_count;
    long     alias_cycle;
    long     alias_depth_exceeded;
    long     conflict;
    long     duplicate;
    long     no_binding;
    long     invalid_command;
    long     invalid_alias;
    long     other;
} Counters;

typedef struct {
    long expected_calls;
    long unexpected_calls;
} HandlerState;

static uint64_t sRngState = 1;

static uint64_t
sNextRandom(void)
{
    /* xorshift64*: deterministic, so any failure is reproducible from --seed. */
    sRngState ^= sRngState >> 12;
    sRngState ^= sRngState << 25;
    sRngState ^= sRngState >> 27;
    return sRngState * UINT64_C(2685821657736338717);
}

static size_t
sRandomBelow(size_t limit)
{
    if (limit == 0) {
        return 0;
    }
    return (size_t)(sNextRandom() % (uint64_t)limit);
}

static UfCommandStatus
sHandler(UfCommandContext *context, const char *command_name, const UfCommandArg *args,
         size_t arg_count, void *user_data)
{
    HandlerState *state = (HandlerState *)user_data;
    (void)context;

    state->expected_calls++;

    /* A handler must be given a coherent slice: a NULL array with a non-zero
     * count, or a count with a NULL array, would be a library bug. */
    if (arg_count > 0 && args == NULL) {
        fprintf(stderr, "stress: handler got a NULL argument array with count %zu\n", arg_count);
        state->unexpected_calls++;
        return UFCOMMAND_HANDLER_ERROR;
    }
    if (command_name == NULL || command_name[0] == '\0') {
        fprintf(stderr, "stress: handler got an empty command name\n");
        state->unexpected_calls++;
        return UFCOMMAND_HANDLER_ERROR;
    }
    for (size_t i = 0; i < arg_count; i++) {
        if (args[i].value == NULL) {
            fprintf(stderr, "stress: handler got a NULL argument at index %zu\n", i);
            state->unexpected_calls++;
            return UFCOMMAND_HANDLER_ERROR;
        }
        if (strlen(args[i].value) != args[i].length) {
            fprintf(stderr, "stress: argument %zu length %zu does not match its value\n", i,
                    args[i].length);
            state->unexpected_calls++;
            return UFCOMMAND_HANDLER_ERROR;
        }
    }
    return UFCOMMAND_OK;
}

static void
sTally(Counters *counters, UfCommandStatus status)
{
    switch (status) {
        case UFCOMMAND_OK:               counters->dispatched++;       break;
        case UFCOMMAND_NOT_FOUND:        counters->not_found++;        break;
        case UFCOMMAND_PARSE_ERROR:      counters->parse_error++;      break;
        case UFCOMMAND_INVALID_ARGUMENT: counters->invalid_argument++; break;
        case UFCOMMAND_ARGUMENT_COUNT:   counters->argument_count++;   break;
        case UFCOMMAND_ALIAS_CYCLE:      counters->alias_cycle++;      break;
        case UFCOMMAND_ALIAS_DEPTH_EXCEEDED: counters->alias_depth_exceeded++; break;
        case UFCOMMAND_CONFLICT:         counters->conflict++;         break;
        case UFCOMMAND_DUPLICATE:        counters->duplicate++;        break;
        case UFCOMMAND_NO_BINDING:       counters->no_binding++;       break;
        case UFCOMMAND_INVALID_COMMAND:  counters->invalid_command++;  break;
        case UFCOMMAND_INVALID_ALIAS:    counters->invalid_alias++;    break;
        default:                         counters->other++;            break;
    }
}

/* Adds a command and requires that it is immediately findable under its own
 * name -- a registration the lookup cannot see is a silent loss. */
static void
sAddAndVerify(UfCommandRegistry *registry, const char *name, HandlerState *handler, Counters *counters)
{
    UfCommandDefinition definition = {name, "", sHandler, handler, 0, SIZE_MAX};
    UfCommandDefinition found;
    UfCommandStatus     status = UfCommandRegistryAdd(registry, &definition);
    if (status != UFCOMMAND_OK) {
        fprintf(stderr, "stress: add '%s' failed with %s\n", name, UfCommandStatusName(status));
        counters->errors++;
        return;
    }
    status = UfCommandRegistryFind(registry, name, &found);
    if (status != UFCOMMAND_OK) {
        fprintf(stderr, "stress: '%s' registered but not found (%s)\n", name, UfCommandStatusName(status));
        counters->errors++;
        return;
    }
    if (strcmp(found.name, name) != 0) {
        fprintf(stderr, "stress: lookup for '%s' returned '%s'\n", name, found.name);
        counters->errors++;
    }
}

static void
sRemoveAndVerify(UfCommandRegistry *registry, const char *name, Counters *counters)
{
    UfCommandDefinition found;
    UfCommandStatus     status = UfCommandRegistryRemove(registry, name);
    if (status != UFCOMMAND_OK && status != UFCOMMAND_NOT_FOUND) {
        fprintf(stderr, "stress: remove '%s' failed with %s\n", name, UfCommandStatusName(status));
        counters->errors++;
        return;
    }
    if (UfCommandRegistryFind(registry, name, &found) != UFCOMMAND_NOT_FOUND) {
        fprintf(stderr, "stress: '%s' removed but still findable\n", name);
        counters->errors++;
    }
}

/* Phase 1: bulk registration across repeated table growth, then verify all. */
static void
sPhaseGrowthAndLookup(UfCommandRegistry *registry, size_t key_count, HandlerState *handler,
                      Counters *counters)
{
    char (*names)[32] = calloc(key_count, 32);
    if (names == NULL) {
        fprintf(stderr, "stress: out of memory for %zu names\n", key_count);
        counters->errors++;
        return;
    }

    for (size_t i = 0; i < key_count; i++) {
        snprintf(names[i], 32, "cmd-%zu", i);
        sAddAndVerify(registry, names[i], handler, counters);
    }
    for (size_t i = 0; i < key_count; i++) {
        UfCommandDefinition found;
        if (UfCommandRegistryFind(registry, names[i], &found) != UFCOMMAND_OK) {
            fprintf(stderr, "stress: '%s' lost after growth\n", names[i]);
            counters->errors++;
        }
    }
    /* Removing every other entry and re-adding forces tombstone reuse on a
     * table that is already at its growth threshold. */
    for (size_t i = 0; i < key_count; i += 2) {
        sRemoveAndVerify(registry, names[i], counters);
    }
    for (size_t i = 0; i < key_count; i += 2) {
        sAddAndVerify(registry, names[i], handler, counters);
    }
    for (size_t i = 0; i < key_count; i++) {
        UfCommandDefinition found;
        if (UfCommandRegistryFind(registry, names[i], &found) != UFCOMMAND_OK) {
            fprintf(stderr, "stress: '%s' lost after tombstone reuse\n", names[i]);
            counters->errors++;
        }
    }
    for (size_t i = 0; i < key_count; i++) {
        sRemoveAndVerify(registry, names[i], counters);
    }

    free(names);
}

/* Phase 2: dispatch lines biased toward names that are actually registered.
 *
 * Uniformly random bytes almost never form a command, so a purely random phase
 * proves only that the tokeniser rejects things.  The generator therefore draws
 * mostly from a real command vocabulary, with arguments and quoting chosen to
 * straddle the argument-count contracts, and keeps a quarter of the iterations
 * fully random for the rejection paths.  A floor on successful dispatches is
 * asserted at the end, so the phase cannot quietly degrade into testing nothing.
 */
static void
sPhaseBiasedDispatch(UfCommandRegistry *registry, UfCommandParser *parser, UfCommandResult *result,
                     HandlerState *handler, Counters *counters, long iterations)
{
    static const char *arguments[] = {"a", "bb", "--flag", "\"two words\"", "\"\"",
                                      "x\\ y", "$1", "*"};
    const size_t vocabulary_size = sizeof(sDispatchVocabulary) / sizeof(sDispatchVocabulary[0]);
    const size_t argument_count = sizeof(arguments) / sizeof(arguments[0]);
    char         line[STRESS_LINE_MAX];
    long         successes = 0;

    /* Register the vocabulary with differing argument contracts so that the
     * generated lines can violate them, rather than every line being accepted. */
    for (size_t i = 0; i < vocabulary_size; i++) {
        UfCommandDefinition definition = {sDispatchVocabulary[i], "", sHandler, handler, 0, i % 3};
        const UfCommandStatus status = UfCommandRegistryAdd(registry, &definition);
        if (status != UFCOMMAND_OK && status != UFCOMMAND_DUPLICATE) {
            fprintf(stderr, "stress: cannot register '%s' for dispatch (%s)\n",
                    sDispatchVocabulary[i], UfCommandStatusName(status));
            counters->errors++;
        }
    }

    for (long iteration = 0; iteration < iterations; iteration++) {
        const size_t variant = sRandomBelow(4);
        size_t       written = 0;

        if (variant == 3) {
            /* Fully random bytes: this is where rejection paths are exercised. */
            const size_t length = sRandomBelow(24);
            for (size_t i = 0; i < length && written + 1 < sizeof line; i++) {
                line[written++] = sRandomAlphabet[sRandomBelow(sizeof(sRandomAlphabet) - 1)];
            }
            line[written] = '\0';
        } else {
            written = (size_t)snprintf(line, sizeof line, "%s",
                                       sDispatchVocabulary[sRandomBelow(vocabulary_size)]);
            const size_t extra = sRandomBelow(4);
            for (size_t i = 0; i < extra; i++) {
                const int appended = snprintf(line + written, sizeof line - written, " %s",
                                              arguments[sRandomBelow(argument_count)]);
                if (appended < 0 || (size_t)appended >= sizeof line - written) {
                    written = sizeof line - 1;
                    break;
                }
                written += (size_t)appended;
            }
            if (variant == 2 && written + 1 < sizeof line) {
                /* A dangling quote or escape: sometimes still valid, often not. */
                line[written++] = sRandomBelow(2) ? '"' : '\\';
                line[written]     = '\0';
            }
        }

        const long before = handler->expected_calls;
        UfCommandStatus status = UfCommandParserExecute(parser, registry, NULL, line, result);
        sTally(counters, status);
        if (status == UFCOMMAND_OK) {
            successes++;
        }

        const bool  reached  = (handler->expected_calls != before);
        const char *resolved = UfCommandResultGetResolvedCommand(result);

        if (status == UFCOMMAND_OK && !reached) {
            fprintf(stderr, "stress: '%s' reported OK without reaching a handler\n", line);
            counters->errors++;
        }
        if (status == UFCOMMAND_OK && resolved == NULL) {
            fprintf(stderr, "stress: '%s' reported OK with no resolved command\n", line);
            counters->errors++;
        }
        if (status != UFCOMMAND_OK && reached) {
            fprintf(stderr, "stress: '%s' reported %s but ran a handler anyway\n", line,
                    UfCommandStatusName(status));
            counters->errors++;
        }
        /* A result whose status disagrees with the returned status would make the
         * accessor useless to a host that only reads the result. */
        if (UfCommandResultGetStatus(result) != status) {
            fprintf(stderr, "stress: '%s' returned %s but the result reports %s\n", line,
                    UfCommandStatusName(status), UfCommandStatusName(UfCommandResultGetStatus(result)));
            counters->errors++;
        }
        if (handler->unexpected_calls > 0) {
            counters->errors++;
            handler->unexpected_calls = 0;
        }
    }

    if (successes < iterations / 20) {
        fprintf(stderr,
                "stress: only %ld of %ld biased lines dispatched; the generator is not reaching "
                "the dispatcher, so this phase proves little\n",
                successes, iterations);
        counters->errors++;
    }
}

/* Phase 3: a random alias graph, deliberately containing cycles and chains, then
 * resolution of every alias.  Nothing here may crash, and nothing may resolve to
 * a command that was never registered. */
static void
sPhaseAliasGraph(UfCommandRegistry *registry, UfCommandParser *parser, UfCommandResult *result,
                 HandlerState *handler, Counters *counters, long alias_count)
{
    char name[16];
    char expansion[64];

    /* Anchors that a valid expansion can legitimately reach. */
    sAddAndVerify(registry, "anchor", handler, counters);

    for (long i = 0; i < alias_count; i++) {
        snprintf(name, sizeof name, "al%ld", i);
        switch (sRandomBelow(6)) {
            case 0:
                snprintf(expansion, sizeof expansion, "anchor $1");
                break;
            case 1:
                snprintf(expansion, sizeof expansion, "anchor $*");
                break;
            case 2:
                /* A reference to a sibling, which may or may not exist yet. */
                snprintf(expansion, sizeof expansion, "al%zu", sRandomBelow((size_t)alias_count));
                break;
            case 3:
                /* A chain two links long. */
                snprintf(expansion, sizeof expansion, "al%zu", sRandomBelow((size_t)alias_count));
                break;
            case 4:
                /* A deliberate cycle: the alias refers to itself. */
                snprintf(expansion, sizeof expansion, "%s", name);
                break;
            default:
                snprintf(expansion, sizeof expansion, "nosuch $3");
                break;
        }
        UfCommandAliasDefinition definition = {name, expansion};
        UfCommandStatus         status = UfCommandRegistryAddAlias(registry, &definition);
        if (status != UFCOMMAND_OK && status != UFCOMMAND_CONFLICT && status != UFCOMMAND_DUPLICATE) {
            fprintf(stderr, "stress: alias '%s' = '%s' rejected with %s\n", name, expansion,
                    UfCommandStatusName(status));
            counters->errors++;
        }
    }

    for (long i = 0; i < alias_count; i++) {
        char line[32];
        snprintf(line, sizeof line, "al%ld x y z", i);

        const long before = handler->expected_calls;
        UfCommandStatus status = UfCommandParserExecute(parser, registry, NULL, line, result);
        sTally(counters, status);

        if (status == UFCOMMAND_OK && handler->expected_calls == before) {
            fprintf(stderr, "stress: alias '%s' reported OK without reaching a handler\n", line);
            counters->errors++;
        }
        if (UfCommandResultGetStatus(result) != status) {
            fprintf(stderr, "stress: alias '%s' result status disagrees with return\n", line);
            counters->errors++;
        }
    }
}

/* Phase 4: tokeniser fuzz over raw bytes, including embedded quotes, escapes and
 * separators.  Only the absence of a crash is asserted; the token budget means
 * most inputs are rejected, which is fine. */
static void
sPhaseTokenizerFuzz(UfCommandRegistry *registry, UfCommandParser *parser, UfCommandResult *result,
                    Counters *counters, long iterations)
{
    static const char bytes[] = "ab \t\n\v\f\r\"\\'$*$1|;&<>(){}[]#%";
    char              line[256];

    for (long iteration = 0; iteration < iterations; iteration++) {
        const size_t length = sRandomBelow(sizeof(line) - 1);
        for (size_t i = 0; i < length; i++) {
            line[i] = bytes[sRandomBelow(sizeof(bytes) - 1)];
        }
        line[length] = '\0';

        UfCommandStatus status = UfCommandParserExecute(parser, registry, NULL, line, result);
        sTally(counters, status);
        UfCommandResultReset(result);
    }
}

static void
sPrintUsage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--iterations N] [--seed N] [--capacity N] [--keys N]\n"
            "  --iterations  random dispatch iterations per phase (default 20000)\n"
            "  --seed        PRNG seed, for reproducing a failure (default 20260921)\n"
            "  --capacity    initial registry capacity (default 16)\n"
            "  --keys        keys registered in the growth phase (default 2048)\n",
            program);
}

int
main(int argc, char **argv)
{
    Options options = {.iterations = 20000, .seed = 20260921, .initial_capacity = 16, .key_count = 2048};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            options.iterations = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            options.seed = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--capacity") == 0 && i + 1 < argc) {
            options.initial_capacity = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            options.key_count = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            sPrintUsage(argv[0]);
            return 0;
        } else {
            sPrintUsage(argv[0]);
            return 2;
        }
    }

    if (options.key_count > STRESS_MAX_KEYS) {
        fprintf(stderr, "stress: --keys is capped at %d\n", STRESS_MAX_KEYS);
        return 2;
    }
    if (options.iterations <= 0 || options.key_count == 0) {
        fprintf(stderr, "stress: --iterations and --keys must be positive\n");
        return 2;
    }

    sRngState = (uint64_t)options.seed | UINT64_C(1);

    UfCommandRegistryConfig registry_config = {options.initial_capacity};
    UfCommandRegistry      *registry = NULL;
    UfCommandParser        *parser = NULL;
    UfCommandResult        *result = NULL;
    HandlerState            handler = {0};
    Counters                counters = {0};

    if (UfCommandRegistryCreate(&registry_config, &registry) != UFCOMMAND_OK ||
        UfCommandParserCreate(NULL, &parser) != UFCOMMAND_OK ||
        UfCommandResultCreate(&result) != UFCOMMAND_OK) {
        fprintf(stderr, "stress: setup failed\n");
        return 1;
    }

    printf("ufcommand stress: seed=%ld iterations=%ld capacity=%zu\n", options.seed, options.iterations,
           options.initial_capacity);

    printf("  phase 1: growth, lookup and tombstone reuse\n");
    sPhaseGrowthAndLookup(registry, options.key_count, &handler, &counters);

    printf("  phase 2: biased dispatch against a registered vocabulary\n");
    sPhaseBiasedDispatch(registry, parser, result, &handler, &counters, options.iterations);

    printf("  phase 3: randomised alias graph\n");
    sPhaseAliasGraph(registry, parser, result, &handler, &counters, 512);

    printf("  phase 4: tokeniser fuzz\n");
    sPhaseTokenizerFuzz(registry, parser, result, &counters, options.iterations);

    printf("  dispatch: %ld ok, %ld not-found, %ld parse-error, %ld bad-argument, %ld bad-arity\n",
           counters.dispatched, counters.not_found, counters.parse_error, counters.invalid_argument,
           counters.argument_count);
    printf("  aliases:  %ld cycle, %ld depth-exceeded, %ld conflict, %ld duplicate\n",
           counters.alias_cycle, counters.alias_depth_exceeded, counters.conflict, counters.duplicate);
    printf("  registry: %ld bad-binding, %ld bad-command, %ld bad-alias, %ld other\n",
           counters.no_binding, counters.invalid_command, counters.invalid_alias, counters.other);
    printf("  handler calls: %ld\n", handler.expected_calls);

    UfCommandResultDestroy(result);
    UfCommandParserDestroy(parser);
    UfCommandRegistryDestroy(registry);

    if (counters.errors != 0) {
        printf("ufcommand stress: FAIL (%ld invariant violation(s))\n", counters.errors);
        return 1;
    }
    printf("ufcommand stress: PASS\n");
    return 0;
}
