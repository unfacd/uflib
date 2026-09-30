/*
 * Exact-oracle integration harness for the ufcommand predictive-completion seam.
 *
 * Black box by construction: nothing here reaches past the public headers, so the
 * oracle asserts what a host can observe -- the candidate set, its order, each
 * candidate's kind, and the replacement range -- and never the engine's private
 * scoring constant.  Candidate order carries the ranking, so the score itself is
 * deliberately not recorded; the design document's Testing section records the
 * trade-off and the score-bearing alternative that was considered and left out.
 *
 * Two layers of checking, on purpose.  The golden comparison catches a change in
 * behaviour; the independent result invariants catch a golden file that was
 * re-recorded from a broken build, which a byte comparison alone can never see.
 *
 * No logger is created: UfLoggerCreateWithDefaults() leaks in the zlog teardown
 * and this binary runs under the dev preset's LeakSanitizer, so attaching that
 * known unrelated leak to these assertions would make a green run meaningless.
 */
#include <uflib/ufcommand/ufcommand.h>
#include <uflib/ufcommand/ufcommand_completion.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GOLDEN_MAX_LINE       8192
#define GOLDEN_MAX_RECORD     32768
#define GOLDEN_MAX_ID         256
#define GOLDEN_MAX_CASES      1024
// Floor on the corpus, so a truncated or swapped-in oracle cannot read as a pass.
// The checked-in corpus holds 48 cases; the margin is for corpus edits, not decay.
#define GOLDEN_MIN_CASES      40
// Published in <uflib/ufcommand/ufcommand_completion.h>: "at most 32".
#define GOLDEN_MAX_CANDIDATES 32

typedef struct GoldenSeenId
{
  char     id[GOLDEN_MAX_ID];
  unsigned line;
} GoldenSeenId;

static GoldenSeenId sSeenIds[GOLDEN_MAX_CASES];
static size_t       sSeenCount = 0;

static UfCommandStatus sHandler(UfCommandContext *context, const char *name, const UfCommandArg *args,
                                size_t argc, void *user_data)
{
  (void)context;
  (void)name;
  (void)args;
  (void)argc;
  (void)user_data;
  return UFCOMMAND_OK;
}

static bool sIsSpace(char c)
{
  return isspace((unsigned char)c) != 0;
}

static void sFatal(const char *what, const char *path)
{
  fprintf(stderr, "fatal: %s '%s': %s\n", what, path, strerror(errno));
  exit(EXIT_FAILURE);
}

// Strips the trailing newline, and any CR a file with CRLF endings would leave.
static void sChomp(char *line)
{
  size_t length = strlen(line);
  while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
}

// 0 at end of stream, 1 for a record, -1 for a line too long for the buffer --
// which must not be mistaken for either of the other two.
static int sReadRecord(FILE *stream, char *buffer, size_t capacity)
{
  if (!fgets(buffer, (int)capacity, stream)) return feof(stream) ? 0 : -1;
  if (!strchr(buffer, '\n') && !feof(stream)) return -1;
  sChomp(buffer);
  return 1;
}

static bool sParseSize(const char *text, size_t *out)
{
  char *               end = NULL;
  unsigned long long   value;
  if (!text || text[0] == '\0') return false;
  errno = 0;
  value = strtoull(text, &end, 10);
  if (errno != 0 || !end || *end != '\0') return false;
  *out = (size_t)value;
  return (unsigned long long)*out == value;
}

// Splits "id<TAB>line<TAB>cursor" in place.  A row that does not have exactly that
// shape is reported as malformed rather than silently coerced into a case.
static bool sParseInputRow(char *buffer, char *id, size_t id_capacity, char *line, size_t line_capacity,
                           size_t *cursor, const char **why)
{
  char *first = strchr(buffer, '\t');
  if (!first) {
    *why = "no field separator";
    return false;
  }
  *first = '\0';
  char *second = strchr(first + 1, '\t');
  if (!second) {
    *why = "no cursor field";
    return false;
  }
  *second = '\0';
  if (second[1] == '\0') {
    *why = "empty cursor field";
    return false;
  }
  if (strlen(buffer) == 0 || strlen(buffer) >= id_capacity) {
    *why = "empty or over-long case id";
    return false;
  }
  if (strlen(first + 1) >= line_capacity) {
    *why = "input line exceeds the harness buffer";
    return false;
  }
  if (!sParseSize(second + 1, cursor)) {
    *why = "cursor is not a number";
    return false;
  }
  if (*cursor > strlen(first + 1)) {
    *why = "cursor is past the end of the line";
    return false;
  }
  strcpy(id, buffer);
  strcpy(line, first + 1);
  return true;
}

static bool sAppend(char *destination, size_t capacity, size_t *used, const char *text)
{
  size_t length = strlen(text);
  if (*used + length + 1 > capacity) return false;
  memcpy(destination + *used, text, length);
  *used += length;
  destination[*used] = '\0';
  return true;
}

static bool sAppendEscaped(char *destination, size_t capacity, size_t *used, const char *text)
{
  char          escape[3];
  unsigned char character;
  while (*text) {
    character = (unsigned char)*text++;
    if (character == '\\' || character == '|' || character == '\t') {
      escape[0] = '\\';
      escape[1] = (character == '\t') ? 't' : (char)character;
      escape[2] = '\0';
      if (!sAppend(destination, capacity, used, escape)) return false;
      continue;
    }
    if (character == '\r' || character == '\n') {
      if (!sAppend(destination, capacity, used, character == '\n' ? "\\n" : "\\r")) return false;
      continue;
    }
    escape[0] = (char)character;
    escape[1] = '\0';
    if (!sAppend(destination, capacity, used, escape)) return false;
  }
  return true;
}

static bool sAppendNumeric(char *destination, size_t capacity, size_t *used, const char *format, ...)
{
  char   buffer[64];
  int    written;
  va_list arguments;

  va_start(arguments, format);
  written = vsnprintf(buffer, sizeof(buffer), format, arguments);
  va_end(arguments);
  if (written < 0 || (size_t)written >= sizeof(buffer)) return false;
  return sAppend(destination, capacity, used, buffer);
}

// count|replacement_start|replacement_end|kind:text;kind:text...
// No score field: the header publishes the ranking as an order, not as a number.
static bool sFormatResult(const UfCommandCompletionResult *result, char *output, size_t capacity)
{
  UfCommandCompletionResultInfo info;
  size_t                        used = 0;

  if (UfCommandCompletionResultGetInfo(result, &info) != UFCOMMAND_OK) return false;
  output[0] = '\0';
  if (!sAppendNumeric(output, capacity, &used, "%zu|%zu|%zu|", info.count, info.replacement_start,
                      info.replacement_end))
    return false;
  for (size_t i = 0; i < info.count; ++i) {
    const UfCommandCompletionCandidate *candidate = UfCommandCompletionResultGetCandidate(result, i);
    if (!candidate) return false;
    if (i != 0 && !sAppend(output, capacity, &used, ";")) return false;
    if (!sAppendNumeric(output, capacity, &used, "%d:", (int)candidate->kind)) return false;
    if (!sAppendEscaped(output, capacity, &used, candidate->text ? candidate->text : "")) return false;
  }
  return true;
}

// Everything the public header promises about a result, checked against the result
// alone.  A golden file recorded from a broken build still has to satisfy these.
static bool sCheckResult(const char *id, const char *line, const UfCommandCompletionResult *result)
{
  UfCommandCompletionResultInfo info;
  bool                          healthy = true;

  if (UfCommandCompletionResultGetInfo(result, &info) != UFCOMMAND_OK) {
    fprintf(stderr, "FAIL %s: the result reports no info\n", id);
    return false;
  }
  size_t length = strlen(line);
  if (info.count > GOLDEN_MAX_CANDIDATES) {
    fprintf(stderr, "FAIL %s: %zu candidates, above the published cap of %d\n", id, info.count,
            GOLDEN_MAX_CANDIDATES);
    healthy = false;
  }
  if (info.replacement_start > info.replacement_end || info.replacement_end > length) {
    fprintf(stderr, "FAIL %s: replacement range [%zu,%zu) is not inside a %zu byte line\n", id,
            info.replacement_start, info.replacement_end, length);
    healthy = false;
  }
  // The dictionary offers aliases only at the first token, so a result reached with
  // a complete token behind it may not carry one.  Derived from the line, not from
  // the engine's own reading of it.
  bool aliases_permitted = true;
  for (size_t i = 0; i < info.replacement_start && i < length; ++i) {
    if (!sIsSpace(line[i])) {
      aliases_permitted = false;
      break;
    }
  }
  bool   have_previous = false;
  double previous      = 0.0;
  for (size_t i = 0; i < info.count; ++i) {
    const UfCommandCompletionCandidate *candidate = UfCommandCompletionResultGetCandidate(result, i);
    if (!candidate || !candidate->text || candidate->text[0] == '\0') {
      fprintf(stderr, "FAIL %s: candidate %zu carries no text\n", id, i);
      healthy = false;
      continue;
    }
    if (candidate->kind < UFCOMMAND_COMPLETION_COMMAND || candidate->kind > UFCOMMAND_COMPLETION_CUSTOM) {
      fprintf(stderr, "FAIL %s: candidate %zu has kind %d outside the enum\n", id, i, (int)candidate->kind);
      healthy = false;
    }
    // Negated comparison: it rejects NaN as well as a negative.
    if (!(candidate->score >= 0.0)) {
      fprintf(stderr, "FAIL %s: candidate %zu scores %f\n", id, i, candidate->score);
      healthy = false;
    }
    if (have_previous && candidate->score > previous) {
      fprintf(stderr, "FAIL %s: candidate %zu outranks its predecessor (%f > %f)\n", id, i,
              candidate->score, previous);
      healthy = false;
    }
    previous      = candidate->score;
    have_previous = true;
    if (candidate->kind == UFCOMMAND_COMPLETION_ALIAS && !aliases_permitted) {
      fprintf(stderr, "FAIL %s: alias '%s' offered past the first token\n", id, candidate->text);
      healthy = false;
    }
    for (size_t j = 0; j < i; ++j) {
      const UfCommandCompletionCandidate *earlier = UfCommandCompletionResultGetCandidate(result, j);
      if (earlier && earlier->text && strcmp(earlier->text, candidate->text) == 0) {
        fprintf(stderr, "FAIL %s: candidate '%s' appears at %zu and %zu\n", id, candidate->text, j, i);
        healthy = false;
      }
    }
  }
  return healthy;
}

// A duplicated case id silently inflates the corpus and hides the case it displaced,
// so it is an oracle defect rather than something to run.
static bool sCheckNotDuplicate(const char *id, unsigned line)
{
  for (size_t i = 0; i < sSeenCount; ++i) {
    if (strcmp(sSeenIds[i].id, id) == 0) {
      fprintf(stderr, "corpus line %u: duplicate case id '%s', first seen at line %u\n", line, id,
              sSeenIds[i].line);
      return false;
    }
  }
  if (sSeenCount < GOLDEN_MAX_CASES) {
    snprintf(sSeenIds[sSeenCount].id, sizeof(sSeenIds[0].id), "%s", id);
    sSeenIds[sSeenCount].line = line;
    ++sSeenCount;
  }
  return true;
}

static bool sBuildFixture(UfCommandRegistry *registry)
{
  static const UfCommandDefinition commands[] = {
      {"remote", "remote operations", sHandler, NULL, 0, SIZE_MAX},
      {"remote add", "add remote", sHandler, NULL, 2, 2},
      {"remote remove", "remove remote", sHandler, NULL, 1, 1},
      {"remote show", "show remote", sHandler, NULL, 0, 0},
      {"reset", "reset state", sHandler, NULL, 0, 0},
      {"status", "show status", sHandler, NULL, 0, 0},
      {"start", "start service", sHandler, NULL, 0, 0},
      {"stop", "stop service", sHandler, NULL, 0, 0},
      {"sync", "synchronize", sHandler, NULL, 0, 0},
  };
  static const UfCommandAliasDefinition aliases[] = {
      {"co", "status"},
      {"rs", "remote show"},
      {"st", "status"},
  };

  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
    if (UfCommandRegistryAdd(registry, &commands[i]) != UFCOMMAND_OK) return false;
  }
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
    if (UfCommandRegistryAddAlias(registry, &aliases[i]) != UFCOMMAND_OK) return false;
  }
  return true;
}

int main(int argc, char **argv)
{
  bool         record       = false;
  const char * input_path   = NULL;
  const char * golden_path  = NULL;
  unsigned     failures     = 0;
  bool         stopped      = false;
  size_t       case_count   = 0;
  FILE *       input        = NULL;
  FILE *       golden       = NULL;
  UfCommandRegistry *registry   = NULL;
  UfCommandCompletion *completion = NULL;

  if (argc == 4 && strcmp(argv[1], "--record") == 0) {
    record      = true;
    input_path  = argv[2];
    golden_path = argv[3];
  } else if (argc == 3) {
    input_path  = argv[1];
    golden_path = argv[2];
  } else {
    fprintf(stderr,
            "usage: %s GOLDEN_INPUT GOLDEN_OUTPUT\n"
            "       %s --record GOLDEN_INPUT GOLDEN_OUTPUT\n"
            "Record mode overwrites the oracle and is a maintainer's tool, never a CI step.\n",
            argv[0], argv[0]);
    return EXIT_FAILURE;
  }

  input = fopen(input_path, "rb");
  if (!input) sFatal("cannot read the corpus", input_path);
  golden = fopen(golden_path, record ? "wb" : "rb");
  if (!golden) sFatal(record ? "cannot write the oracle" : "cannot read the oracle", golden_path);

  UfCommandRegistryConfig config = {0};
  config.initial_capacity        = 32;
  config.completion_enabled      = true;
  if (UfCommandRegistryCreate(&config, &registry) != UFCOMMAND_OK) {
    fprintf(stderr, "fatal: the registry could not be created\n");
    return EXIT_FAILURE;
  }
  if (!sBuildFixture(registry)) {
    fprintf(stderr, "fatal: the command fixture could not be registered\n");
    UfCommandRegistryDestroy(registry);
    return EXIT_FAILURE;
  }
  completion = UfCommandRegistryGetCompletion(registry);
  if (!completion) {
    fprintf(stderr, "fatal: completion_enabled produced no engine\n");
    UfCommandRegistryDestroy(registry);
    return EXIT_FAILURE;
  }

  for (;;) {
    char        buffer[GOLDEN_MAX_LINE];
    char        id[GOLDEN_MAX_ID];
    char        line[GOLDEN_MAX_LINE];
    char        expected[GOLDEN_MAX_RECORD];
    char        actual[GOLDEN_MAX_RECORD];
    const char *why    = NULL;
    size_t      cursor = 0;
    int         read   = sReadRecord(input, buffer, sizeof(buffer));

    if (read == 0) break;
    ++case_count;
    // A defect in the checked-in corpus or oracle is not a finding about the engine,
    // and carrying on would pair the wrong records against each other -- burying one
    // real report under a cascade of mismatches.  So these stop the run.
    if (read < 0) {
      fprintf(stderr, "corpus line %zu: a line past the harness buffer -- stopping\n", case_count);
      ++failures;
      stopped = true;
      break;
    }
    if (!sParseInputRow(buffer, id, sizeof(id), line, sizeof(line), &cursor, &why)) {
      fprintf(stderr, "corpus line %zu: %s -- stopping\n", case_count, why);
      ++failures;
      stopped = true;
      break;
    }
    if (!sCheckNotDuplicate(id, (unsigned)case_count)) {
      ++failures;
      stopped = true;
      break;
    }

    if (!record) {
      int golden_read = sReadRecord(golden, expected, sizeof(expected));
      if (golden_read <= 0) {
        fprintf(stderr, "%s: the oracle %s -- stopping\n", id,
                golden_read == 0 ? "has no record for this case" : "holds a line past the harness buffer");
        ++failures;
        stopped = true;
        break;
      }
    }

    UfCommandCompletionResult *result = NULL;
    UfCommandStatus            status = UfCommandCompletionSuggest(completion, line, cursor, &result);
    if (status != UFCOMMAND_OK || !result) {
      fprintf(stderr, "FAIL %s: completion returned %s\n", id, UfCommandStatusName(status));
      ++failures;
      continue;
    }
    if (!sCheckResult(id, line, result)) {
      ++failures;
      UfCommandCompletionResultDestroy(result);
      continue;
    }
    if (!sFormatResult(result, actual, sizeof(actual))) {
      fprintf(stderr, "FAIL %s: the result record does not fit %d bytes\n", id, GOLDEN_MAX_RECORD);
      ++failures;
      UfCommandCompletionResultDestroy(result);
      continue;
    }
    if (record) {
      fprintf(golden, "%s\n", actual);
    } else if (strcmp(actual, expected) != 0) {
      fprintf(stderr, "FAIL %s\n  input:    <%s> @ %zu\n  expected: %s\n  actual:   %s\n", id, line,
              cursor, expected, actual);
      ++failures;
    }
    UfCommandCompletionResultDestroy(result);
  }

  // Both describe the corpus and oracle as a whole, so they say nothing once the
  // run has already stopped on a defect in one of them.
  if (!record && !stopped) {
    char extra[GOLDEN_MAX_RECORD];
    int  trailing = sReadRecord(golden, extra, sizeof(extra));
    if (trailing != 0) {
      fprintf(stderr, "FAIL: the oracle holds a record no case consumes: %s\n",
              trailing > 0 ? extra : "<line past the harness buffer>");
      ++failures;
    }
    if (case_count < GOLDEN_MIN_CASES) {
      fprintf(stderr,
              "FAIL: the corpus holds %zu case(s); at least %d are required, so this file is "
              "truncated or is not the corpus\n",
              case_count, GOLDEN_MIN_CASES);
      ++failures;
    }
  }

  fclose(input);
  fclose(golden);
  UfCommandRegistryDestroy(registry);
  printf("golden completion: %zu cases, %u failures\n", case_count, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
