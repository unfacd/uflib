/*
 * Adversarial tests for the ufcommand predictive-completion seam, and for the
 * registry state that has to survive UfCommandRegistryLoadUserConfig()'s swap.
 *
 * These are written to break the implementation, not to lock in what it happens
 * to do: each case is named for the defect it would catch.
 *
 * White-box in one respect: the telemetry counters are read through the module's
 * private header, because the public interface exposes them as JSON alone.  That
 * matches ufcommand_telemetry_tests.c.
 *
 * No logger is created here on purpose.  UfLoggerCreateWithDefaults() /
 * UfLoggerDestroy() leak ~121 KB in the zlog teardown (design document §8.2 #3,
 * owned by src/logger/), and this suite runs under the dev preset's
 * LeakSanitizer -- attaching that known leak to these assertions would make a
 * green run mean nothing.
 */
#include <uflib/ufcommand/ufcommand.h>
#include <uflib/ufcommand/ufcommand_telemetry.h>
#include <uflib/ufcommand/ufcommand_completion.h>
#include "ufcommand_telemetry_priv.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The config is passed by pointer at create time, so its layout is an ABI
// surface: completion_enabled sits at offset 17, in existing padding, to hold
// this at 32.  If this fires, the addition was misplaced.
_Static_assert(sizeof(UfCommandRegistryConfig) == 32, "UfCommandRegistryConfig grew past its published 32 bytes");

#define CONFIG_PATH "ufcommand_completion_tests.conf.lua"
#define JSON_PATH   "ufcommand_completion_tests.json"

static UfCommandStatus
sHandler(UfCommandContext *context, const char *name, const UfCommandArg *args, size_t arg_count, void *user_data)
{
  (void)context; (void)name; (void)args; (void)arg_count; (void)user_data;
  return UFCOMMAND_OK;
}

static void
sWriteFile(const char *path, const char *text)
{
  FILE *f = fopen(path, "wb");
  assert(f != NULL);
  assert(fputs(text, f) >= 0);
  assert(fclose(f) == 0);
}

static uint64_t
sCommandCount(UfCommandTelemetry *telemetry, const char *name)
{
  uint64_t count = 0;
  assert(UfCommandTelemetryGetCommandCount(telemetry, name, &count) == UFCOMMAND_OK);
  return count;
}

// Every pointer and counter a refused load must leave alone.
static void
sAssertStateIntact(UfCommandRegistry *registry, UfCommandTelemetry *telemetry, uint64_t status_count,
                   UfCommandModifier expected_modifiers)
{
  UfCommandKeyConfig keys = { UFCOMMAND_MOD_NONE };
  assert(UfCommandRegistryGetTelemetry(registry) == telemetry);
  assert(sCommandCount(telemetry, "status") == status_count);
  assert(UfCommandRegistryGetKeyConfig(registry, &keys) == UFCOMMAND_OK);
  assert(keys.default_modifiers == expected_modifiers);
}

/* ── The registry swap ─────────────────────────────────────────────────────── */

static void
sTestCarryOver(void)
{
  UfCommandRegistry *registry = NULL;
  UfCommandParser   *parser   = NULL;
  UfCommandResult   *result   = NULL;

  UfCommandRegistryConfig config = { 0 };
  config.telemetry_enabled = true;
  assert(UfCommandRegistryCreate(&config, &registry) == UFCOMMAND_OK);
  assert(UfCommandParserCreate(NULL, &parser) == UFCOMMAND_OK);
  assert(UfCommandResultCreate(&result) == UFCOMMAND_OK);

  UfCommandDefinition definitions[] = {
    { "status",     "show state", sHandler, NULL, 0, 0 },
    { "remote add", "add remote", sHandler, NULL, 2, 2 },
  };
  for (size_t i = 0; i < sizeof(definitions) / sizeof(definitions[0]); ++i) {
    assert(UfCommandRegistryAdd(registry, &definitions[i]) == UFCOMMAND_OK);
  }
  UfCommandAliasDefinition alias = { "ra", "remote add $1 $2" };
  assert(UfCommandRegistryAddAlias(registry, &alias) == UFCOMMAND_OK);

  UfCommandTelemetry *telemetry = UfCommandRegistryGetTelemetry(registry);
  assert(telemetry != NULL);

  assert(UfCommandParserExecute(parser, registry, NULL, "status", result) == UFCOMMAND_OK);
  sAssertStateIntact(registry, telemetry, 1, UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT);

  UfCommandKeyConfig keys = { UFCOMMAND_MOD_ALT };
  assert(UfCommandRegistrySetKeyConfig(registry, &keys) == UFCOMMAND_OK);

  // Refused loads return before the swap, so nothing may change.  These run
  // before the successful load so a pass cannot be an ordering artefact.
  assert(UfCommandRegistryLoadUserConfig(registry, "no_such_directory/none.lua") == UFCOMMAND_IO_ERROR);
  sAssertStateIntact(registry, telemetry, 1, UFCOMMAND_MOD_ALT);

  sWriteFile(CONFIG_PATH, "return {\n  version = 2,\n  aliases = { [\"st\"] = \"status\" },\n}\n");
  assert(UfCommandRegistryLoadUserConfig(registry, CONFIG_PATH) == UFCOMMAND_UNSUPPORTED_VERSION);
  sAssertStateIntact(registry, telemetry, 1, UFCOMMAND_MOD_ALT);

  sWriteFile(CONFIG_PATH, "return {\n  version = 1,\n  aliases = { [\"dz\"] = \"status\", [\"dz\"] = \"status\" },\n}\n");
  assert(UfCommandRegistryLoadUserConfig(registry, CONFIG_PATH) == UFCOMMAND_DUPLICATE);
  sAssertStateIntact(registry, telemetry, 1, UFCOMMAND_MOD_ALT);

  // A successful load rebuilds the registry from its entries and copies the
  // result over the caller's struct, so anything not carried is dropped and the
  // outgoing contents -- the observer among them -- go to Destroy().  Identity,
  // continuity and visibility are what carry weight here; a "no leak" assertion
  // would be worthless, because the observer is destroyed rather than leaked.
  sWriteFile(CONFIG_PATH, "return {\n  version = 1,\n  aliases = { [\"st\"] = \"status\" },\n}\n");
  assert(UfCommandRegistryLoadUserConfig(registry, CONFIG_PATH) == UFCOMMAND_OK);
  assert(UfCommandRegistryGetTelemetry(registry) == telemetry);

  assert(UfCommandParserExecute(parser, registry, NULL, "status", result) == UFCOMMAND_OK);
  sAssertStateIntact(registry, telemetry, 2, UFCOMMAND_MOD_ALT);

  // Visibility: the loaded configuration actually took effect.
  assert(UfCommandParserExecute(parser, registry, NULL, "st", result) == UFCOMMAND_OK);
  assert(UfCommandParserExecute(parser, registry, NULL, "ra origin git@example.com", result) == UFCOMMAND_OK);
  assert(sCommandCount(telemetry, "status") == 3);

  // The one public telemetry call, which returned INVALID_ARGUMENT once the
  // observer was dropped.
  assert(UfCommandTelemetrySaveJson(telemetry, JSON_PATH) == UFCOMMAND_OK);
  remove(JSON_PATH);

  // A reload is DUPLICATE, not CONFLICT: the staging clone already carries the
  // alias, so AddAlias sees a name already taken by an alias.
  assert(UfCommandRegistryLoadUserConfig(registry, CONFIG_PATH) == UFCOMMAND_DUPLICATE);
  sAssertStateIntact(registry, telemetry, 3, UFCOMMAND_MOD_ALT);

  // CONFLICT is the other half of the shared namespace, and is what protects a
  // registered command from being written over by a user file.
  sWriteFile(CONFIG_PATH, "return {\n  version = 1,\n  aliases = { [\"status\"] = \"remote add $1 $2\" },\n}\n");
  assert(UfCommandRegistryLoadUserConfig(registry, CONFIG_PATH) == UFCOMMAND_CONFLICT);
  sAssertStateIntact(registry, telemetry, 3, UFCOMMAND_MOD_ALT);

  // A second successful load must not disturb the handle or accumulate observers.
  sWriteFile(CONFIG_PATH, "return {\n  version = 1,\n  aliases = { [\"dz\"] = \"status\" },\n}\n");
  assert(UfCommandRegistryLoadUserConfig(registry, CONFIG_PATH) == UFCOMMAND_OK);
  assert(UfCommandRegistryGetTelemetry(registry) == telemetry);
  assert(sCommandCount(telemetry, "status") == 3);

  remove(CONFIG_PATH);

  // The registry owns the observer, so there is nothing to destroy separately.
  UfCommandResultDestroy(result);
  UfCommandParserDestroy(parser);
  UfCommandRegistryDestroy(registry);
}

/* ── Providers the tests drive the engine with ─────────────────────────────── */

typedef struct sSingle
{
  const char *            text;
  const char *            display;
  const char *            description;
  UfCommandCompletionKind kind;
  double                  score;
} sSingle;

static UfCommandStatus
sEmitSingle(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit, void *emit_context)
{
  const sSingle *spec = user_data;
  (void)query;
  UfCommandCompletionCandidate candidate = { spec->text, spec->display, spec->description, spec->kind, spec->score };
  return emit(emit_context, &candidate);
}

static UfCommandStatus
sEmitNothing(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit, void *emit_context)
{
  (void)user_data; (void)query; (void)emit; (void)emit_context;
  return UFCOMMAND_OK;
}

static UfCommandStatus
sEmitMalformed(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit, void *emit_context)
{
  (void)user_data; (void)query;
  UfCommandCompletionCandidate candidate = { NULL, NULL, NULL, UFCOMMAND_COMPLETION_CUSTOM, 1.0 };
  return emit(emit_context, &candidate);
}

static UfCommandStatus
sEmitEmptyText(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit, void *emit_context)
{
  (void)user_data; (void)query;
  UfCommandCompletionCandidate candidate = { "", "d", "e", UFCOMMAND_COMPLETION_CUSTOM, 1.0 };
  return emit(emit_context, &candidate);
}

static UfCommandStatus
sFail(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit, void *emit_context)
{
  (void)user_data; (void)query; (void)emit; (void)emit_context;
  return UFCOMMAND_CANCELLED;
}

// The candidate points at a buffer that dies with this call, so a result that
// still reads correctly afterwards proves the engine copied rather than borrowed.
static UfCommandStatus
sEmitFlood(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit, void *emit_context)
{
  size_t times = *(const size_t *)user_data;
  (void)query;
  for (size_t i = 0; i < times; ++i) {
    char   text[32];
    snprintf(text, sizeof(text), "flood%05zu", i);
    UfCommandCompletionCandidate candidate = { text, NULL, NULL, UFCOMMAND_COMPLETION_CUSTOM, 1.0 };
    UfCommandStatus             status    = emit(emit_context, &candidate);
    if (status != UFCOMMAND_OK) return status;
  }
  return UFCOMMAND_OK;
}

typedef struct sProbe
{
  UfCommandCompletion *engine;
  UfCommandRegistry *  registry;
  UfCommandStatus      observed;
} sProbe;

// Re-enters the engine from inside a provider: the failure mode is unbounded
// recursion, each frame carrying a component scratch buffer.
static UfCommandStatus
sRecurse(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit, void *emit_context)
{
  sProbe *probe  = user_data;
  (void)query; (void)emit; (void)emit_context;
  UfCommandCompletionResult *result = NULL;
  probe->observed                   = UfCommandCompletionSuggest(probe->engine, "re", 2, &result);
  UfCommandCompletionResultDestroy(result);
  return UFCOMMAND_OK;
}

// Registers a provider into the loop that is running it.
static UfCommandStatus
sAddDuringSuggest(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit,
                  void *emit_context)
{
  sProbe *                        probe = user_data;
  (void)query; (void)emit; (void)emit_context;
  UfCommandCompletionProviderDefinition late = { "late", sEmitNothing, NULL };
  probe->observed                            = UfCommandCompletionAddProvider(probe->engine, &late);
  return UFCOMMAND_OK;
}

// Mutates the registry underneath the provider that is reading it.
static UfCommandStatus
sMutateDuringSuggest(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit,
                     void *emit_context)
{
  sProbe *           probe = user_data;
  (void)query; (void)emit; (void)emit_context;
  UfCommandDefinition sneaky = { "sneaky", "x", sHandler, NULL, 0, SIZE_MAX };
  probe->observed            = UfCommandRegistryAdd(probe->registry, &sneaky);
  return UFCOMMAND_OK;
}

// The destructive one: LoadUserConfig frees and replaces every table, so a
// provider calling it would leave the dictionary provider walking freed arrays.
static UfCommandStatus
sReloadDuringSuggest(void *user_data, const UfCommandCompletionQuery *query, UfCommandCompletionEmit emit,
                     void *emit_context)
{
  sProbe *probe = user_data;
  (void)query; (void)emit; (void)emit_context;
  probe->observed = UfCommandRegistryLoadUserConfig(probe->registry, CONFIG_PATH);
  return UFCOMMAND_OK;
}

static const UfCommandCompletionCandidate *
sFind(const UfCommandCompletionResult *result, const char *text)
{
  for (size_t i = 0; i < UfCommandCompletionResultGetCount(result); ++i) {
    const UfCommandCompletionCandidate *candidate = UfCommandCompletionResultGetCandidate(result, i);
    if (strcmp(candidate->text, text) == 0) return candidate;
  }
  return NULL;
}

static UfCommandRegistry *
sNewRegistry(UfCommandCompletion **out_completion, bool completion_enabled, bool telemetry_enabled)
{
  UfCommandRegistryConfig config = { 0 };
  config.completion_enabled = completion_enabled;
  config.telemetry_enabled  = telemetry_enabled;
  UfCommandRegistry *registry = NULL;
  assert(UfCommandRegistryCreate(&config, &registry) == UFCOMMAND_OK);
  *out_completion = UfCommandRegistryGetCompletion(registry);
  assert(completion_enabled == (*out_completion != NULL));
  return registry;
}

/* ── The completion seam ───────────────────────────────────────────────────── */

static void
sTestOptOutAndNullTolerance(void)
{
  // A registry that never asked carries no engine, so there is nothing to check
  // before asking for one and nothing to pay for.
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, false, false);
  assert(completion == NULL);

  UfCommandCompletionProviderDefinition provider = { "p", sEmitSingle, NULL };
  assert(UfCommandCompletionAddProvider(NULL, &provider) == UFCOMMAND_INVALID_ARGUMENT);

  UfCommandCompletionResult *result = NULL;
  assert(UfCommandCompletionSuggest(NULL, "x", 1, &result) == UFCOMMAND_INVALID_ARGUMENT);
  assert(result == NULL);

  UfCommandCompletionResultDestroy(NULL);
  UfCommandCompletionResultInfo info = { 1, 1, 1 };
  assert(UfCommandCompletionResultGetInfo(NULL, &info) == UFCOMMAND_INVALID_ARGUMENT);
  assert(UfCommandCompletionResultGetCount(NULL) == 0);
  assert(UfCommandCompletionResultGetCandidate(NULL, 0) == NULL);

  UfCommandRegistryDestroy(registry);

  // The same entry points, reached through an engine that does exist.
  registry = sNewRegistry(&completion, true, false);
  assert(UfCommandCompletionSuggest(completion, NULL, 0, &result) == UFCOMMAND_INVALID_ARGUMENT);
  assert(result == NULL);
  assert(UfCommandCompletionAddProvider(completion, NULL) == UFCOMMAND_INVALID_ARGUMENT);
  assert(UfCommandCompletionResultGetInfo(NULL, NULL) == UFCOMMAND_INVALID_ARGUMENT);
  UfCommandRegistryDestroy(registry);
}

static void
sTestDictionaryCompletion(void)
{
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);

  UfCommandDefinition definitions[] = {
    { "status",         "show state",   sHandler, NULL, 0, SIZE_MAX },
    { "remote",         "remotes",      sHandler, NULL, 0, SIZE_MAX },
    { "remote add",     "add remote",   sHandler, NULL, 0, SIZE_MAX },
    { "remote remove",  "drop remote",  sHandler, NULL, 0, SIZE_MAX },
    { "config get key", "read a key",   sHandler, NULL, 0, SIZE_MAX },
    { "cx",             NULL,           sHandler, NULL, 0, SIZE_MAX },
  };
  for (size_t i = 0; i < sizeof(definitions) / sizeof(definitions[0]); ++i) {
    assert(UfCommandRegistryAdd(registry, &definitions[i]) == UFCOMMAND_OK);
  }
  UfCommandAliasDefinition aliases[] = { { "co", "status" }, { "zz", "status" } };
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
    assert(UfCommandRegistryAddAlias(registry, &aliases[i]) == UFCOMMAND_OK);
  }

  UfCommandCompletionResult *result = NULL;

  // Three registered names share the component "remote", and it is offered once.
  assert(UfCommandCompletionSuggest(completion, "re", 2, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(result) == 1);
  assert(strcmp(UfCommandCompletionResultGetCandidate(result, 0)->text, "remote") == 0);
  UfCommandCompletionResultDestroy(result);

  // An exact match is still offered, so a host can decide whether to skip it.
  assert(UfCommandCompletionSuggest(completion, "status", 6, &result) == UFCOMMAND_OK);
  assert(sFind(result, "status") != NULL);
  UfCommandCompletionResultDestroy(result);

  // A trailing space completes the next component, never a whole replacement.
  assert(UfCommandCompletionSuggest(completion, "remote ", 7, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(result) == 2);
  assert(strcmp(UfCommandCompletionResultGetCandidate(result, 0)->text, "add") == 0);
  assert(strcmp(UfCommandCompletionResultGetCandidate(result, 1)->text, "remove") == 0);
  UfCommandCompletionResultDestroy(result);

  // A command outranks an alias at the same level.
  assert(UfCommandCompletionSuggest(completion, "c", 1, &result) == UFCOMMAND_OK);
  const UfCommandCompletionCandidate *command = sFind(result, "config");
  const UfCommandCompletionCandidate *alias   = sFind(result, "co");
  assert(command != NULL && alias != NULL);
  assert(command->kind == UFCOMMAND_COMPLETION_COMMAND);
  assert(alias->kind == UFCOMMAND_COMPLETION_ALIAS);
  assert(command->score > alias->score);
  UfCommandCompletionResultDestroy(result);

  // An alias is a top-level name, so it is not offered past the first token --
  // including one that would otherwise prefix-match a component.
  assert(UfCommandCompletionSuggest(completion, "cx c", 4, &result) == UFCOMMAND_OK);
  assert(sFind(result, "co") == NULL);
  UfCommandCompletionResultDestroy(result);

  // A command registered with no description still reports one.
  assert(UfCommandCompletionSuggest(completion, "cx", 2, &result) == UFCOMMAND_OK);
  const UfCommandCompletionCandidate *bare = sFind(result, "cx");
  assert(bare != NULL && bare->description != NULL);
  UfCommandCompletionResultDestroy(result);

  // Matching nothing is empty, not an error, and must not sort a NULL array.
  assert(UfCommandCompletionSuggest(completion, "zzzzz", 5, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(result) == 0);
  assert(UfCommandCompletionResultGetCandidate(result, 0) == NULL);
  UfCommandCompletionResultDestroy(result);

  // A repeated component appears once, however many names carry it.
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ "set set", "s", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_OK);
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ "set set set", "s", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_OK);
  assert(UfCommandCompletionSuggest(completion, "set s", 5, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(result) == 1);
  UfCommandCompletionResultDestroy(result);

  UfCommandRegistryDestroy(registry);
}

static void
sTestReplacementRange(void)
{
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ "remote add", "a", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_OK);

  UfCommandCompletionResult *    result = NULL;
  UfCommandCompletionResultInfo  info   = { 0, 0, 0 };
  const char *                   line   = "remote add";

  // Cursor at the end of the token: the range is the token.
  assert(UfCommandCompletionSuggest(completion, line, 10, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetInfo(result, &info) == UFCOMMAND_OK);
  assert(info.replacement_start == 7 && info.replacement_end == 10);
  UfCommandCompletionResultDestroy(result);

  // Cursor mid-token.  The range still spans the whole token: [7,8) would drop
  // the trailing "d" and complete the line into "remote dadd".
  assert(UfCommandCompletionSuggest(completion, line, 8, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetInfo(result, &info) == UFCOMMAND_OK);
  assert(info.replacement_start == 7 && info.replacement_end == 10);
  assert(sFind(result, "add") != NULL);
  UfCommandCompletionResultDestroy(result);

  // Cursor at a token boundary completes the token it starts.
  assert(UfCommandCompletionSuggest(completion, line, 7, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetInfo(result, &info) == UFCOMMAND_OK);
  assert(info.replacement_start == 7 && info.replacement_end == 10);
  UfCommandCompletionResultDestroy(result);

  // Inside a whitespace run the active token is empty: an insertion point.
  assert(UfCommandCompletionSuggest(completion, "remote  add", 7, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetInfo(result, &info) == UFCOMMAND_OK);
  assert(info.replacement_start == 7 && info.replacement_end == 7);
  UfCommandCompletionResultDestroy(result);

  // Past the end is rejected, and the out-parameter is still cleared.
  assert(UfCommandCompletionSuggest(completion, line, 11, &result) == UFCOMMAND_INVALID_ARGUMENT);
  assert(result == NULL);

  // Degenerate lines: an empty one, one that is all whitespace, and a cursor at
  // zero all complete from the top level without reading outside the line.
  const char *degenerate[] = { "", "   ", " " };
  for (size_t i = 0; i < sizeof(degenerate) / sizeof(degenerate[0]); ++i) {
    size_t length = strlen(degenerate[i]);
    assert(UfCommandCompletionSuggest(completion, degenerate[i], length, &result) == UFCOMMAND_OK);
    assert(UfCommandCompletionResultGetInfo(result, &info) == UFCOMMAND_OK);
    assert(info.replacement_start == length && info.replacement_end == length);
    UfCommandCompletionResultDestroy(result);
  }
  // An empty line completes every top-level name, which is the right answer
  // rather than an empty one.
  assert(UfCommandCompletionSuggest(completion, "", 0, &result) == UFCOMMAND_OK);
  assert(sFind(result, "remote") != NULL);
  UfCommandCompletionResultDestroy(result);

  // A high byte in the token must not be read as whitespace through a signed
  // char, and the offsets stay byte offsets.
  assert(UfCommandCompletionSuggest(completion, "re\xc3\xa9", 4, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetInfo(result, &info) == UFCOMMAND_OK);
  assert(info.replacement_start == 0 && info.replacement_end == 4);
  UfCommandCompletionResultDestroy(result);

  UfCommandRegistryDestroy(registry);
}

static void
sTestCanonicalisedCommandPrefix(void)
{
  // The dispatcher tokenises, so all of these run.  A command prefix sliced from
  // the raw line would carry the doubled whitespace and match no canonical name,
  // completing nothing on a line that works.
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ "config get key", "k", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_OK);

  const char *lines[] = { "config  get  k", "config\tget\tk", "config \t get  k" };
  for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
    UfCommandCompletionResult *result = NULL;
    assert(UfCommandCompletionSuggest(completion, lines[i], strlen(lines[i]), &result) == UFCOMMAND_OK);
    assert(UfCommandCompletionResultGetCount(result) == 1);
    assert(strcmp(UfCommandCompletionResultGetCandidate(result, 0)->text, "key") == 0);
    UfCommandCompletionResultDestroy(result);
  }

  // Leading whitespace is not part of any token, and must not shift the index:
  // "g" still resolves to component 1 of the name, not component 0.
  UfCommandCompletionResult *result = NULL;
  assert(UfCommandCompletionSuggest(completion, "   config g", 11, &result) == UFCOMMAND_OK);
  assert(sFind(result, "get") != NULL);
  UfCommandCompletionResultDestroy(result);

  UfCommandRegistryDestroy(registry);
}

static void
sTestCandidateRanking(void)
{
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);

  // Enough matching commands to fill the candidate cap, so an arrival-ordered
  // cap would freeze a host provider out entirely.
  for (size_t i = 0; i < 40; ++i) {
    char name[16];
    snprintf(name, sizeof(name), "aaa%02zu", i);
    UfCommandDefinition definition = { name, "filler", sHandler, NULL, 0, SIZE_MAX };
    assert(UfCommandRegistryAdd(registry, &definition) == UFCOMMAND_OK);
  }

  sSingle winner = { "zzz", "zzz", "host winner", UFCOMMAND_COMPLETION_CUSTOM, 1.0e9 };
  UfCommandCompletionProviderDefinition host = { "host", sEmitSingle, &winner };
  assert(UfCommandCompletionAddProvider(completion, &host) == UFCOMMAND_OK);

  UfCommandCompletionResult *result = NULL;
  assert(UfCommandCompletionSuggest(completion, "aa", 2, &result) == UFCOMMAND_OK);
  // Capped, but capped by score: the dictionary's 40 matches must not have taken
  // the slots before the host provider was asked.
  assert(UfCommandCompletionResultGetCount(result) == 32);
  assert(strcmp(UfCommandCompletionResultGetCandidate(result, 0)->text, "zzz") == 0);
  assert(sFind(result, "zzz") != NULL);
  UfCommandCompletionResultDestroy(result);

  UfCommandRegistryDestroy(registry);
}

static void
sTestCandidateMergeIsDeterministic(void)
{
  // Two contributors offering the same text.  Whichever wins must not depend on
  // which provider ran first, or the result is untestable and unpinnable.
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);

  sSingle high = { "dup", "bbb", "from the first provider", UFCOMMAND_COMPLETION_CUSTOM, 5.0 };
  sSingle low  = { "dup", "aaa", "from the second provider", UFCOMMAND_COMPLETION_ALIAS, 5.0 };
  UfCommandCompletionProviderDefinition first  = { "first", sEmitSingle, &high };
  UfCommandCompletionProviderDefinition second = { "second", sEmitSingle, &low };
  assert(UfCommandCompletionAddProvider(completion, &first) == UFCOMMAND_OK);
  assert(UfCommandCompletionAddProvider(completion, &second) == UFCOMMAND_OK);

  UfCommandCompletionResult *result = NULL;
  assert(UfCommandCompletionSuggest(completion, "d", 1, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(result) == 1);
  const UfCommandCompletionCandidate *merged = UfCommandCompletionResultGetCandidate(result, 0);
  assert(strcmp(merged->text, "dup") == 0);
  assert(strcmp(merged->display, "aaa") == 0);
  assert(merged->kind == UFCOMMAND_COMPLETION_ALIAS);
  UfCommandCompletionResultDestroy(result);

  // The higher score wins outright, whoever offered it.
  sSingle stronger = { "dup2", "zzz", "lower", UFCOMMAND_COMPLETION_CUSTOM, 1.0 };
  sSingle weaker   = { "dup2", "aaa", "higher", UFCOMMAND_COMPLETION_CUSTOM, 9.0 };
  UfCommandCompletionProviderDefinition stronger_provider = { "strong", sEmitSingle, &stronger };
  UfCommandCompletionProviderDefinition weaker_provider   = { "weak", sEmitSingle, &weaker };
  assert(UfCommandCompletionAddProvider(completion, &stronger_provider) == UFCOMMAND_OK);
  assert(UfCommandCompletionAddProvider(completion, &weaker_provider) == UFCOMMAND_OK);
  assert(UfCommandCompletionSuggest(completion, "d", 1, &result) == UFCOMMAND_OK);
  assert(strcmp(sFind(result, "dup2")->description, "higher") == 0);
  UfCommandCompletionResultDestroy(result);

  UfCommandRegistryDestroy(registry);
}

static void
sTestProviderAbuse(void)
{
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);

  // A well-formed definition, so the NULL-engine case below is invalid for
  // exactly one reason.
  sSingle                              spec  = { "x", NULL, NULL, UFCOMMAND_COMPLETION_CUSTOM, 1.0 };
  UfCommandCompletionProviderDefinition valid = { "valid", sEmitSingle, &spec };
  assert(UfCommandCompletionAddProvider(NULL, &valid) == UFCOMMAND_INVALID_ARGUMENT);

  UfCommandCompletionProviderDefinition bad = { "p", NULL, NULL };
  assert(UfCommandCompletionAddProvider(completion, &bad) == UFCOMMAND_INVALID_ARGUMENT);
  bad = (UfCommandCompletionProviderDefinition){ NULL, sEmitSingle, &spec };
  assert(UfCommandCompletionAddProvider(completion, &bad) == UFCOMMAND_INVALID_ARGUMENT);
  bad = (UfCommandCompletionProviderDefinition){ "", sEmitSingle, &spec };
  assert(UfCommandCompletionAddProvider(completion, &bad) == UFCOMMAND_INVALID_ARGUMENT);
  // Refused before the engine was touched, so the registry is untouched too --
  // and it is released here, because every case below builds its own.
  UfCommandRegistryDestroy(registry);

  UfCommandCompletionResult *result = NULL;

  // Each abusing provider gets its own engine.  Providers run in the order they
  // were added, so sharing one engine would let an earlier abort mask every
  // case after it -- the same reason each asserts its own status.
  struct
  {
    UfCommandCompletionProvider provider;
    UfCommandStatus             expected;
  } abusive[] = {
    { sFail,          UFCOMMAND_CANCELLED        },
    { sEmitMalformed, UFCOMMAND_INVALID_ARGUMENT },
    { sEmitEmptyText, UFCOMMAND_INVALID_ARGUMENT },
  };
  for (size_t i = 0; i < sizeof(abusive) / sizeof(abusive[0]); ++i) {
    registry = sNewRegistry(&completion, true, false);
    UfCommandCompletionProviderDefinition provider = { "abusive", abusive[i].provider, NULL };
    assert(UfCommandCompletionAddProvider(completion, &provider) == UFCOMMAND_OK);
    // A malformed candidate or a failing provider aborts the call and yields no
    // result at all, rather than a partial one.
    result = (UfCommandCompletionResult *)0x1;
    assert(UfCommandCompletionSuggest(completion, "nothing", 7, &result) == abusive[i].expected);
    assert(result == NULL);
    UfCommandCompletionResultDestroy(result);
    UfCommandRegistryDestroy(registry);
  }

  // A provider that contributes nothing is not an error.
  registry = sNewRegistry(&completion, true, false);
  UfCommandCompletionProviderDefinition quiet = { "quiet", sEmitNothing, NULL };
  assert(UfCommandCompletionAddProvider(completion, &quiet) == UFCOMMAND_OK);
  assert(UfCommandCompletionSuggest(completion, "nothing", 7, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(result) == 0);
  UfCommandCompletionResultDestroy(result);

  // The cap admits seven host providers; the registry's dictionary holds the
  // eighth slot, so the refusal below is the eighth host provider.
  for (size_t i = 0; i < 6; ++i) {
    UfCommandCompletionProviderDefinition filler = { "filler", sEmitNothing, NULL };
    assert(UfCommandCompletionAddProvider(completion, &filler) == UFCOMMAND_OK);
  }
  UfCommandCompletionProviderDefinition over = { "over", sEmitNothing, NULL };
  assert(UfCommandCompletionAddProvider(completion, &over) == UFCOMMAND_BUFFER_TOO_SMALL);

  // ...and the refusal must not have cost the engine its dictionary provider,
  // which is the one provider a host cannot put back.
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ "status", "s", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_OK);
  assert(UfCommandCompletionSuggest(completion, "st", 2, &result) == UFCOMMAND_OK);
  assert(sFind(result, "status") != NULL);
  UfCommandCompletionResultDestroy(result);

  UfCommandRegistryDestroy(registry);
}

static void
sTestCandidateSanitisation(void)
{
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);

  // A negative or NaN score would break the ordering, so both read as zero.
  sSingle bad_scores[] = {
    { "neg", "neg", "negative", UFCOMMAND_COMPLETION_CUSTOM, -5.0 },
    { "nan", "nan", "not a number", UFCOMMAND_COMPLETION_CUSTOM, nan("") },
  };
  for (size_t i = 0; i < 2; ++i) {
    UfCommandCompletionProviderDefinition provider = { "bad", sEmitSingle, &bad_scores[i] };
    assert(UfCommandCompletionAddProvider(completion, &provider) == UFCOMMAND_OK);
  }
  sSingle positive = { "pos", "pos", "positive", UFCOMMAND_COMPLETION_CUSTOM, 7.0 };
  UfCommandCompletionProviderDefinition provider = { "pos", sEmitSingle, &positive };
  assert(UfCommandCompletionAddProvider(completion, &provider) == UFCOMMAND_OK);

  UfCommandCompletionResult *result = NULL;
  assert(UfCommandCompletionSuggest(completion, "n", 1, &result) == UFCOMMAND_OK);
  // Sorted and finite: the positive candidate leads, and neither score is NaN,
  // so the comparator stays a strict weak ordering.
  assert(strcmp(UfCommandCompletionResultGetCandidate(result, 0)->text, "pos") == 0);
  for (size_t i = 0; i < UfCommandCompletionResultGetCount(result); ++i) {
    assert(!isnan(UfCommandCompletionResultGetCandidate(result, i)->score));
    assert(UfCommandCompletionResultGetCandidate(result, i)->score >= 0.0);
  }
  // The defaulted presentation fields are never NULL, even when not offered.
  const UfCommandCompletionCandidate *nan_candidate = sFind(result, "nan");
  assert(nan_candidate != NULL && nan_candidate->display != NULL && nan_candidate->description != NULL);
  UfCommandCompletionResultDestroy(result);

  UfCommandRegistryDestroy(registry);
}

static void
sTestCandidateFloodAndOwnership(void)
{
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);

  size_t times = 10000;
  UfCommandCompletionProviderDefinition flood = { "flood", sEmitFlood, &times };
  assert(UfCommandCompletionAddProvider(completion, &flood) == UFCOMMAND_OK);

  UfCommandCompletionResult *result = NULL;
  assert(UfCommandCompletionSuggest(completion, "f", 1, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(result) == 32);
  // The candidates pointed at a stack buffer that died with the provider call,
  // so reading them here is the ownership contract under AddressSanitizer.
  for (size_t i = 0; i < UfCommandCompletionResultGetCount(result); ++i) {
    const UfCommandCompletionCandidate *candidate = UfCommandCompletionResultGetCandidate(result, i);
    assert(strncmp(candidate->text, "flood", 5) == 0);
    assert(candidate->display != NULL && candidate->description != NULL);
  }
  // Out-of-range accessors answer NULL rather than reading past the array.
  assert(UfCommandCompletionResultGetCandidate(result, 32) == NULL);
  assert(UfCommandCompletionResultGetCandidate(result, SIZE_MAX) == NULL);
  UfCommandCompletionResultDestroy(result);

  UfCommandRegistryDestroy(registry);
}

static void
sTestLiveResultsAreIndependent(void)
{
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ "status", "s", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_OK);

  UfCommandCompletionResult *first = NULL;
  assert(UfCommandCompletionSuggest(completion, "st", 2, &first) == UFCOMMAND_OK);

  // Handing the same slot back in must overwrite it without freeing what was
  // there: the caller owns it, and a call that freed it would be a double free.
  UfCommandCompletionResult *slot = first;
  assert(UfCommandCompletionSuggest(completion, "st", 2, &slot) == UFCOMMAND_OK);
  assert(slot != first);
  assert(UfCommandCompletionResultGetCount(first) == 1);

  UfCommandCompletionResultDestroy(slot);
  UfCommandCompletionResultDestroy(first);

  UfCommandRegistryDestroy(registry);
}

static void
sTestContainment(void)
{
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ "status", "s", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_OK);

  // A provider is arbitrary host code.  The registry already refuses mutation
  // from inside a handler, and a suggestion holds the same guard, so a provider
  // cannot reconfigure the dictionary it was called to describe.
  // One probe per provider, so no assertion depends on which ran first.
  sProbe mutate_probe  = { completion, registry, UFCOMMAND_OK };
  sProbe add_probe     = { completion, registry, UFCOMMAND_OK };
  sProbe reload_probe  = { completion, registry, UFCOMMAND_OK };
  sProbe recurse_probe = { completion, registry, UFCOMMAND_OK };

  sWriteFile(CONFIG_PATH, "return {\n  version = 1,\n  aliases = { [\"q\"] = \"status\" },\n}\n");
  UfCommandCompletionProviderDefinition providers[] = {
    { "mutate",  sMutateDuringSuggest, &mutate_probe  },
    { "add",     sAddDuringSuggest,    &add_probe     },
    { "reload",  sReloadDuringSuggest, &reload_probe  },
    { "recurse", sRecurse,             &recurse_probe },
  };
  for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); ++i) {
    assert(UfCommandCompletionAddProvider(completion, &providers[i]) == UFCOMMAND_OK);
  }

  UfCommandCompletionResult *result = NULL;
  assert(UfCommandCompletionSuggest(completion, "st", 2, &result) == UFCOMMAND_OK);
  UfCommandCompletionResultDestroy(result);

  assert(mutate_probe.observed == UFCOMMAND_CONFLICT);
  assert(add_probe.observed == UFCOMMAND_CONFLICT);
  // The widest-reaching one: LoadUserConfig replaces every table, so a provider
  // allowed to call it would swap the dictionary out from under the suggestion
  // running right then -- and its result would describe a registry that no
  // longer exists.
  assert(reload_probe.observed == UFCOMMAND_CONFLICT);
  // Re-entering the engine is bounded rather than unbounded recursion.
  assert(recurse_probe.observed == UFCOMMAND_CONFLICT);

  // The refusals must not have left the registry unusable, and the configuration
  // the refused provider asked for must not have been applied halfway.
  assert(UfCommandRegistryLoadUserConfig(registry, CONFIG_PATH) == UFCOMMAND_OK);
  assert(UfCommandCompletionSuggest(completion, "q", 1, &result) == UFCOMMAND_OK);
  assert(sFind(result, "q") != NULL);
  UfCommandCompletionResultDestroy(result);
  remove(CONFIG_PATH);

  UfCommandRegistryDestroy(registry);
}

static void
sTestLongestNameIsCompletable(void)
{
  // The engine reads a command component into a fixed buffer, which would be a
  // silent way to make a command uncompletable.  It is sized above the longest
  // name the registry will store, and this pins that: a 4095-byte name is both
  // registerable and completable, and one byte longer cannot be registered at
  // all, so it can never reach the engine.
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, false);

  char *fits = malloc(4095 + 1);
  assert(fits != NULL);
  memset(fits, 'x', 4095);
  fits[4095] = '\0';
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ fits, "d", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_OK);

  UfCommandCompletionResult *result = NULL;
  assert(UfCommandCompletionSuggest(completion, "xx", 2, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(result) == 1);
  assert(strcmp(UfCommandCompletionResultGetCandidate(result, 0)->text, fits) == 0);
  UfCommandCompletionResultDestroy(result);
  free(fits);

  char *over = malloc(4096 + 1);
  assert(over != NULL);
  memset(over, 'y', 4096);
  over[4096] = '\0';
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ over, "d", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_INVALID_COMMAND);
  free(over);

  UfCommandRegistryDestroy(registry);

  // Nothing registered at all completes to nothing, without reading anywhere.
  UfCommandRegistry *empty = sNewRegistry(&completion, true, false);
  assert(UfCommandCompletionSuggest(completion, "st", 2, &result) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(result) == 0);
  UfCommandCompletionResultDestroy(result);
  UfCommandRegistryDestroy(empty);
}

static void
sTestCompletionSurvivesConfigLoad(void)
{
  // The engine reads the registry live rather than snapshotting it, and it is
  // the same engine on the other side of the swap.  If this fails, the design is
  // wrong at the architectural level and no amount of patching fixes it.
  UfCommandCompletion *completion = NULL;
  UfCommandRegistry   *registry   = sNewRegistry(&completion, true, true);
  assert(UfCommandRegistryAdd(registry, &(UfCommandDefinition){ "status", "s", sHandler, NULL, 0, SIZE_MAX }) ==
         UFCOMMAND_OK);

  UfCommandCompletionResult *before = NULL;
  assert(UfCommandCompletionSuggest(completion, "before", 6, &before) == UFCOMMAND_OK);
  assert(UfCommandCompletionResultGetCount(before) == 0);

  sWriteFile(CONFIG_PATH, "return {\n  version = 1,\n  aliases = { [\"q\"] = \"status\" },\n}\n");
  assert(UfCommandRegistryLoadUserConfig(registry, CONFIG_PATH) == UFCOMMAND_OK);
  remove(CONFIG_PATH);

  assert(UfCommandRegistryGetCompletion(registry) == completion);

  // Visibility: the loaded alias is completable, so the engine followed the swap.
  UfCommandCompletionResult *after = NULL;
  assert(UfCommandCompletionSuggest(completion, "q", 1, &after) == UFCOMMAND_OK);
  assert(sFind(after, "q") != NULL);
  UfCommandCompletionResultDestroy(after);

  // A result taken before the swap is still readable after it: the engine copied
  // the strings rather than borrowing the registry's.
  assert(UfCommandCompletionResultGetCount(before) == 0);
  UfCommandCompletionResultDestroy(before);

  UfCommandRegistryDestroy(registry);
}

int
main(void)
{
  sTestCarryOver();
  sTestOptOutAndNullTolerance();
  sTestDictionaryCompletion();
  sTestReplacementRange();
  sTestCanonicalisedCommandPrefix();
  sTestCandidateRanking();
  sTestCandidateMergeIsDeterministic();
  sTestProviderAbuse();
  sTestCandidateSanitisation();
  sTestCandidateFloodAndOwnership();
  sTestLiveResultsAreIndependent();
  sTestContainment();
  sTestLongestNameIsCompletable();
  sTestCompletionSurvivesConfigLoad();
  puts("completion tests passed");
  return 0;
}
