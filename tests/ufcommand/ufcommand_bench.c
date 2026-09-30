/**
 * @file ufcommand_bench.c
 * @brief Reproducible microbenchmark for the ufcommand module.
 *
 * Every figure reported in the design document's Performance section is produced
 * by this program, so a change to the module can be re-measured the same way
 * rather than compared against numbers nobody can regenerate.
 *
 * Standalone and deliberately **not** registered with CTest: it is a measurement
 * tool, and a timing assertion in CI is a flaky test rather than a test.  Build
 * it with optimisations on and sanitizers off — a sanitizer build measures the
 * sanitizer.  The supported route is the `release` preset:
 *
 *   cmake --preset release -D_PACKAGE_TESTS=ON
 *   cmake --build build/release --target ufcommand_bench -j"$(nproc)"
 *   ./build/release/tests/ufcommand/ufcommand_bench --keys 65536 --reps 400000
 *
 * The absolute numbers are machine-dependent; the shape is what is worth
 * tracking.  Run it three times and take a median before believing a small
 * difference — the dispatch figures in particular vary by ~10% run to run.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L   /* clock_gettime; the build may already set it */
#endif

#include <uflib/ufcommand/ufcommand.h>
#include <uflib/ufcommand/ufcommand_completion.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The table sizes the insertion sweep walks.  They straddle several growth
 * steps, which is where the amortised cost is visible. */
static const size_t sScales[] = {256, 1024, 4096, 16384, 65536};

static double
sNowNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static UfCommandStatus
sHandler(UfCommandContext *ctx, const char *name, const UfCommandArg *args, size_t argc, void *ud)
{
    (void)ctx; (void)name; (void)args; (void)argc; (void)ud;
    return UFCOMMAND_OK;
}

static void
sAdd(UfCommandRegistry *registry, const char *name)
{
    UfCommandDefinition def = {name, "description", sHandler, NULL, 0, SIZE_MAX};
    UfCommandRegistryAdd(registry, &def);
}

/* ── 1. Insertion: average, and the worst single insert (a growth step) ────── */

static void
sBenchInsertion(void)
{
    printf("\n-- insertion: average and worst single insert\n");
    printf("%10s %12s %12s %14s\n", "keys", "avg ns", "ops/s", "worst insert");

    for (size_t s = 0; s < sizeof(sScales) / sizeof(sScales[0]); ++s) {
        const size_t count = sScales[s];
        char (*names)[24] = malloc(count * 24);
        if (names == NULL) { printf("  out of memory\n"); return; }

        UfCommandRegistry *registry = NULL;
        UfCommandRegistryCreate(NULL, &registry);

        double worst = 0.0;
        const double start = sNowNs();
        for (size_t i = 0; i < count; ++i) {
            snprintf(names[i], 24, "cmd-%zu", i);
            const double one = sNowNs();
            sAdd(registry, names[i]);
            const double delta = sNowNs() - one;
            if (delta > worst) { worst = delta; }
        }
        const double total = sNowNs() - start;

        printf("%10zu %12.1f %12.0f %11.1f us\n", count, total / (double)count,
               1e9 / (total / (double)count), worst / 1000.0);

        free(names);
        UfCommandRegistryDestroy(registry);
    }
}

/* ── 2. Command lookup: hit and miss at the post-insert load factor ───────── */

static void
sBenchFind(size_t count)
{
    char (*names)[24] = malloc(count * 24);
    char *absent = malloc(count * 24);
    if (names == NULL || absent == NULL) { printf("  out of memory\n"); free(names); free(absent); return; }

    UfCommandRegistry *registry = NULL;
    UfCommandRegistryCreate(NULL, &registry);
    for (size_t i = 0; i < count; ++i) {
        snprintf(names[i], 24, "cmd-%zu", i);
        sAdd(registry, names[i]);
    }

    UfCommandDefinition out;
    const size_t reps = 200000;
    double start = sNowNs();
    for (size_t i = 0; i < reps; ++i) {
        UfCommandRegistryFind(registry, names[i % count], &out);
    }
    const double hit = (sNowNs() - start) / (double)reps;

    start = sNowNs();
    for (size_t i = 0; i < reps; ++i) {
        snprintf(absent, 24, "zzz-%zu", i);
        UfCommandRegistryFind(registry, absent, &out);
    }
    const double miss = (sNowNs() - start) / (double)reps;

    printf("\n-- Find over %zu commands (canonicalising path)\n", count);
    printf("   hit %.1f ns    miss %.1f ns\n", hit, miss);

    free(names); free(absent);
    UfCommandRegistryDestroy(registry);
}

/* ── 3. Dispatch: tokenise + resolve + prefix match + handler ─────────────── */

static void
sBenchDispatch(const char *label, const char *line, UfCommandParser *parser,
               UfCommandRegistry *registry, UfCommandResult *result, size_t reps)
{
    for (size_t i = 0; i < 1000; ++i) {           /* warm up */
        UfCommandParserExecute(parser, registry, NULL, line, result);
    }
    const double start = sNowNs();
    for (size_t i = 0; i < reps; ++i) {
        UfCommandParserExecute(parser, registry, NULL, line, result);
    }
    printf("   %-28s %8.1f ns/op\n", label, (sNowNs() - start) / (double)reps);
}

static void
sBenchDispatches(size_t reps)
{
    UfCommandRegistry *registry = NULL;
    UfCommandParser   *parser   = NULL;
    UfCommandResult   *result   = NULL;
    UfCommandRegistryCreate(NULL, &registry);
    UfCommandParserCreate(NULL, &parser);
    UfCommandResultCreate(&result);

    sAdd(registry, "status");
    sAdd(registry, "checkout");
    sAdd(registry, "remote");
    sAdd(registry, "remote add");
    const UfCommandAliasDefinition aliases[] = {
        {"co", "checkout $1"},      /* one hop              */
        {"b",  "co $1"},            /* two hops             */
        {"a",  "b $*"},             /* three hops, $* carry */
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        UfCommandRegistryAddAlias(registry, &aliases[i]);
    }

    printf("\n-- dispatch\n");
    sBenchDispatch("direct, one token",     "status",                  parser, registry, result, reps);
    sBenchDispatch("direct, two tokens",    "remote add origin url",   parser, registry, result, reps);
    sBenchDispatch("longest prefix, 4 tok", "remote add x y",          parser, registry, result, reps);
    sBenchDispatch("alias, one hop",        "co main",                 parser, registry, result, reps);
    sBenchDispatch("alias chain, two hops", "b main",                  parser, registry, result, reps);
    sBenchDispatch("alias chain, 3 hops",   "a main",                  parser, registry, result, reps);

    UfCommandResultDestroy(result);
    UfCommandParserDestroy(parser);
    UfCommandRegistryDestroy(registry);
}

/* ── 4. Bindings: the table whose slot stride does not divide a cache line ─── */

static void
sBenchBindings(size_t keys, size_t reps)
{
    char (*names)[24] = malloc(keys * 24);
    if (names == NULL) { printf("  out of memory\n"); return; }

    UfCommandRegistry *registry = NULL;
    UfCommandParser   *parser   = NULL;
    UfCommandResult   *result   = NULL;
    UfCommandRegistryCreate(NULL, &registry);
    UfCommandParserCreate(NULL, &parser);
    UfCommandResultCreate(&result);
    sAdd(registry, "status");

    double start = sNowNs();
    for (size_t i = 0; i < keys; ++i) {
        snprintf(names[i], 24, "key-%zu", i);
        UfCommandBinding binding = {UFCOMMAND_MOD_CTRL, names[i], "status"};
        UfCommandRegistryAddBinding(registry, &binding);
    }
    const double insert = (sNowNs() - start) / (double)keys;

    /* A real parser is passed deliberately: with a NULL parser the entry point
     * returns on its own argument check and the table is never probed at all. */
    start = sNowNs();
    for (size_t i = 0; i < reps; ++i) {
        UfCommandParserExecuteBinding(parser, registry, NULL, UFCOMMAND_MOD_CTRL,
                                      names[i % keys], result);
    }
    const double hit = (sNowNs() - start) / (double)reps;

    /* A miss is the clean probe measurement: no binding is found, so nothing is
     * dispatched and the cost is the probe plus the status return. */
    char absent[32];
    start = sNowNs();
    for (size_t i = 0; i < reps; ++i) {
        snprintf(absent, sizeof(absent), "absent-%zu", i);
        UfCommandParserExecuteBinding(parser, registry, NULL, UFCOMMAND_MOD_CTRL, absent, result);
    }
    const double miss = (sNowNs() - start) / (double)reps;

    start = sNowNs();
    for (size_t i = 0; i < keys; ++i) {
        UfCommandRegistryRemoveBinding(registry, UFCOMMAND_MOD_CTRL, names[i]);
        UfCommandBinding binding = {UFCOMMAND_MOD_CTRL, names[i], "status"};
        UfCommandRegistryAddBinding(registry, &binding);
    }
    const double churn = (sNowNs() - start) / (double)keys;

    printf("\n-- bindings over %zu keys\n", keys);
    printf("   insert %.1f ns    dispatch hit %.1f ns    probe miss %.1f ns    remove+readd %.1f ns\n",
           insert, hit, miss, churn);

    free(names);
    UfCommandResultDestroy(result);
    UfCommandParserDestroy(parser);
    UfCommandRegistryDestroy(registry);
}

/* ── 5. Add/remove churn: does tombstone reuse keep the table from growing? ── */

static void
sBenchChurn(void)
{
    const size_t rounds = 20000;
    UfCommandRegistry *registry = NULL;
    UfCommandRegistryCreate(NULL, &registry);

    const double start = sNowNs();
    for (size_t i = 0; i < rounds; ++i) {
        sAdd(registry, "churn-target");
        UfCommandRegistryRemove(registry, "churn-target");
    }
    printf("\n-- add+remove churn, one live entry\n");
    printf("   %.1f ns/cycle over %zu cycles\n", (sNowNs() - start) / (double)rounds, rounds);

    UfCommandRegistryDestroy(registry);
}

/* ── 5. Completion: one full pass over the command table per call ─────────── */

static void
sBenchCompletion(const char *label, size_t keys, size_t reps)
{
    char (*names)[24] = malloc(keys * 24);
    if (names == NULL) { printf("  out of memory\n"); return; }

    UfCommandRegistryConfig config = { 0 };
    config.completion_enabled = true;
    UfCommandRegistry *registry = NULL;
    UfCommandRegistryCreate(&config, &registry);
    for (size_t i = 0; i < keys; ++i) {
        snprintf(names[i], 24, "cmd%05zu", i);
        UfCommandDefinition definition = { names[i], "generated", sHandler, NULL, 0, SIZE_MAX };
        UfCommandRegistryAdd(registry, &definition);
    }
    UfCommandCompletion *completion = UfCommandRegistryGetCompletion(registry);
    if (completion == NULL) { printf("  completion unavailable\n"); free(names); UfCommandRegistryDestroy(registry); return; }

    /* Each call allocates its result and walks the whole table, so this cannot be
     * driven as hard as the dispatch loops; the per-call figure is what matters. */
    size_t rounds = reps < 3000 ? reps : 3000;
    printf("\n-- completion (%s: %zu commands)\n", label, keys);

    /* A prefix that matches many, one that matches few, and one that matches
     * nothing: the scan is the same length in all three. */
    const char *prefixes[] = {"cmd00", "cmd000", "nothing"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        UfCommandCompletionResult *result = NULL;
        for (size_t w = 0; w < 20; ++w) {
            if (UfCommandCompletionSuggest(completion, prefixes[i], strlen(prefixes[i]), &result) != UFCOMMAND_OK) break;
            UfCommandCompletionResultDestroy(result);
        }
        UfCommandCompletionResultInfo info = { 0, 0, 0 };
        UfCommandCompletionSuggest(completion, prefixes[i], strlen(prefixes[i]), &result);
        UfCommandCompletionResultGetInfo(result, &info);
        UfCommandCompletionResultDestroy(result);

        const double start = sNowNs();
        for (size_t r = 0; r < rounds; ++r) {
            if (UfCommandCompletionSuggest(completion, prefixes[i], strlen(prefixes[i]), &result) != UFCOMMAND_OK) break;
            UfCommandCompletionResultDestroy(result);
        }
        char line[64];
        snprintf(line, sizeof(line), "%s (%zu kept)", prefixes[i], info.count);
        printf("   %-28s %8.1f us/call\n", line, (sNowNs() - start) / (double)rounds / 1000.0);
    }

    free(names);
    UfCommandRegistryDestroy(registry);
}

static void
sUsage(const char *program)
{
    fprintf(stderr, "usage: %s [--keys N] [--reps N] [--quick]\n", program);
}

int
main(int argc, char **argv)
{
    size_t keys = 65536;
    size_t reps = 400000;
    int    quick = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            keys = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            reps = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--quick") == 0) {
            quick = 1;
        } else {
            sUsage(argv[0]);
            return 2;
        }
    }
    if (keys == 0 || reps == 0) { sUsage(argv[0]); return 2; }
    if (quick) { keys = 4096; reps = 50000; }

    printf("ufcommand benchmark: keys=%zu reps=%zu  (build with -O2, no sanitizers)\n", keys, reps);

    sBenchInsertion();
    sBenchFind(keys < 20000 ? keys : 100000);
    sBenchDispatches(reps);
    sBenchBindings(keys < 4096 ? keys : 4096, reps);
    sBenchChurn();
    sBenchCompletion("large", keys, reps);
    sBenchCompletion("typical", 256, reps);
    return 0;
}
