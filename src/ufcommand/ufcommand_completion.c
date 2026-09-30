#include <uflib/ufcommand/ufcommand_completion.h>
#include "ufcommand_completion_priv.h"
#include "ufcommand_priv.h"
#include "ufcommand_type_priv.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// The candidate cap is a ranking bound, not an arrival bound: the weakest held
// candidate is what a new one has to beat, so a host provider can outrank the
// dictionary however many commands match.
#define UFCOMMAND_COMPLETION_MAX_CANDIDATES 32
// Host providers only.  The registry's own dictionary provider holds slot 0.
#define UFCOMMAND_COMPLETION_MAX_PROVIDERS 8
// Sized above the longest name UfCommandRegistryAdd() will store -- sValidSimpleName
// rejects 4096 bytes or more -- so no registered command is ever unsuggestable,
// and the per-keystroke path stays free of heap traffic.
#define UFCOMMAND_COMPLETION_COMPONENT_MAX 4096

struct UfCommandCompletion
{
  UfCommandRegistry *                    dictionary; // BORROWED; the owning registry
  UfCommandCompletionProviderDefinition  providers[UFCOMMAND_COMPLETION_MAX_PROVIDERS];
  size_t                                 provider_count;
  unsigned                               suggest_depth; // re-entrancy guard
};

struct UfCommandCompletionResult
{
  UfCommandCompletionCandidate *items;
  size_t                        count;
  size_t                        replacement_start, replacement_end;
};

typedef struct UfCommandCompletionEmitContext
{
  UfCommandCompletionResult *result;
} UfCommandCompletionEmitContext;

static bool sIsSpace(char c)
{
  return isspace((unsigned char)c) != 0;
}

static bool sHasPrefix(const char *text, const char *prefix)
{
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void sFreeCandidate(UfCommandCompletionCandidate *candidate)
{
  free((void *)candidate->text);
  free((void *)candidate->display);
  free((void *)candidate->description);
  memset(candidate, 0, sizeof(*candidate));
}

// The one ordering this module defines, used both to pick a winner between two
// candidates carrying the same text and to sort the result.  Score descending,
// then text, display and kind ascending -- a total order over everything the
// engine can hold, so a result never depends on the order providers ran in or on
// the order the registry stores its entries in.
static int sCandidateCompare(const void *left, const void *right)
{
  const UfCommandCompletionCandidate *a = left;
  const UfCommandCompletionCandidate *b = right;
  if (a->score > b->score) return -1;
  if (a->score < b->score) return 1;
  int order = strcmp(a->text, b->text);
  if (order != 0) return order;
  order = strcmp(a->display ? a->display : "", b->display ? b->display : "");
  if (order != 0) return order;
  if (a->kind != b->kind) return (int)a->kind - (int)b->kind;
  return 0;
}

static UfCommandStatus sStoreCandidate(UfCommandCompletionCandidate *slot, const UfCommandCompletionCandidate *source)
{
  sFreeCandidate(slot);
  slot->text        = UfCommandStrDup(source->text);
  slot->display     = UfCommandStrDup(source->display);
  slot->description = UfCommandStrDup(source->description);
  if (!slot->text || !slot->display || !slot->description) {
    sFreeCandidate(slot);
    return UFCOMMAND_NO_MEMORY;
  }
  slot->kind  = source->kind;
  slot->score = source->score;
  return UFCOMMAND_OK;
}

static UfCommandStatus sEmit(void *context, const UfCommandCompletionCandidate *candidate)
{
  UfCommandCompletionEmitContext *emit_context = context;
  UfCommandCompletionResult *     result       = emit_context->result;
  if (!candidate || !candidate->text || candidate->text[0] == '\0') return UFCOMMAND_INVALID_ARGUMENT;
  UfCommandCompletionCandidate incoming = *candidate;
  // A negative or NaN score breaks the ordering, so it is read as zero.
  if (!(incoming.score >= 0.0)) incoming.score = 0.0;
  if (!incoming.display) incoming.display = incoming.text;
  if (!incoming.description) incoming.description = "";

  size_t slot = result->count;
  for (size_t i = 0; i < result->count; ++i) {
    if (strcmp(result->items[i].text, incoming.text) == 0) {
      slot = i;
      break;
    }
  }
  if (slot == result->count) {
    if (result->count == UFCOMMAND_COMPLETION_MAX_CANDIDATES) {
      // Full: find the weakest held candidate, which is the only one a new
      // arrival may displace.  sCandidateCompare puts the weaker operand last.
      slot = 0;
      for (size_t i = 1; i < result->count; ++i) {
        if (sCandidateCompare(&result->items[i], &result->items[slot]) > 0) slot = i;
      }
      // Judged before allocating: a candidate that loses is dropped for free.
      if (sCandidateCompare(&incoming, &result->items[slot]) >= 0) return UFCOMMAND_OK;
    } else {
      ++result->count;
    }
  } else if (sCandidateCompare(&incoming, &result->items[slot]) >= 0) {
    return UFCOMMAND_OK;
  }
  return sStoreCandidate(&result->items[slot], &incoming);
}

static void sFreeResult(UfCommandCompletionResult *result)
{
  if (!result) return;
  for (size_t i = 0; i < result->count; ++i) sFreeCandidate(&result->items[i]);
  free(result->items);
  free(result);
}

// The component at 'index' of a canonical name, when it starts with 'prefix'.
// NULL covers both "this name has no such component" and a component too long
// for the buffer, which the registry's own name limit makes unreachable.
static const char *sNextComponent(const char *name, const char *prefix, size_t index, char *buffer, size_t buffer_size)
{
  size_t position = 0, component = 0, length = strlen(name);
  while (position < length) {
    while (position < length && sIsSpace(name[position])) ++position;
    if (position == length) break;
    size_t begin = position;
    while (position < length && !sIsSpace(name[position])) ++position;
    if (component == index) {
      size_t size = position - begin;
      if (size + 1 > buffer_size) return NULL;
      memcpy(buffer, name + begin, size);
      buffer[size] = '\0';
      return sHasPrefix(buffer, prefix) ? buffer : NULL;
    }
    ++component;
  }
  return NULL;
}

// A registered name matches the command prefix when it is that prefix or extends
// it at a component boundary, so "remote" does not match "remotely".
static bool sCommandHasPrefix(const char *name, const char *command_prefix)
{
  if (command_prefix[0] == '\0') return true;
  size_t length = strlen(command_prefix);
  if (strncmp(name, command_prefix, length) != 0) return false;
  return name[length] == '\0' || sIsSpace(name[length]);
}

// Exact before prefix, command before alias, shorter text first -- the order the
// public header documents, with the length term too small to disturb either rank.
static double sDictionaryScore(const char *text, const char *prefix, bool is_command)
{
  double score = is_command ? 100.0 : 0.0;
  if (strcmp(text, prefix) == 0) score += 200.0;
  return score - (double)strlen(text) / 1000.0;
}

static UfCommandStatus sDictionaryProvider(void *provider_context, const UfCommandCompletionQuery *query,
                                           UfCommandCompletionEmit emit, void *emit_context)
{
  UfCommandRegistry *registry = provider_context;
  if (!registry || !query || !emit) return UFCOMMAND_INVALID_ARGUMENT;
  char component[UFCOMMAND_COMPLETION_COMPONENT_MAX];
  for (size_t i = 0; i < registry->command_capacity; ++i) {
    UfCommandEntryPrivate *entry = &registry->commands[i];
    if (!entry->occupied) continue;
    if (!sCommandHasPrefix(entry->name, query->command_prefix)) continue;
    const char *next = sNextComponent(entry->name, query->token_prefix, query->token_index, component,
                                      sizeof(component));
    if (!next) continue;
    // Only the next component is offered, never a whole replacement command.
    UfCommandCompletionCandidate candidate = { next, entry->name, entry->description,
                                               UFCOMMAND_COMPLETION_COMMAND,
                                               sDictionaryScore(next, query->token_prefix, true) };
    UfCommandStatus status = emit(emit_context, &candidate);
    if (status != UFCOMMAND_OK) return status;
  }
  // Aliases are top-level only, so they are offered at the first token alone.
  if (query->token_index == 0 && query->command_prefix[0] == '\0') {
    for (size_t i = 0; i < registry->alias_capacity; ++i) {
      UfCommandAliasPrivate *entry = &registry->aliases[i];
      if (!entry->occupied || !sHasPrefix(entry->name, query->token_prefix)) continue;
      UfCommandCompletionCandidate candidate = { entry->name, entry->name, entry->expansion,
                                                 UFCOMMAND_COMPLETION_ALIAS,
                                                 sDictionaryScore(entry->name, query->token_prefix, false) };
      UfCommandStatus status = emit(emit_context, &candidate);
      if (status != UFCOMMAND_OK) return status;
    }
  }
  return UFCOMMAND_OK;
}

// The active token is the run of non-whitespace around the cursor, bounded by
// whitespace or the end of the line.  It is the whole run, not the part before
// the cursor: replacing a half-token would otherwise mangle its tail.
static UfCommandStatus sBuildQuery(const char *line, size_t cursor, UfCommandCompletionQuery *query,
                                   char **out_token_prefix, char **out_command_prefix, size_t *out_start,
                                   size_t *out_end)
{
  size_t length = strlen(line);
  if (cursor > length) return UFCOMMAND_INVALID_ARGUMENT;
  size_t start = cursor;
  while (start > 0 && !sIsSpace(line[start - 1])) --start;
  size_t end = cursor;
  while (end < length && !sIsSpace(line[end])) ++end;

  char *token_prefix = UfCommandStrNDup(line + start, cursor - start);
  if (!token_prefix) return UFCOMMAND_NO_MEMORY;

  // The prefix is rebuilt from the tokens rather than sliced, because the
  // dispatcher tokenises: a raw slice of "config  get" carries a doubled space
  // and would match no canonical name at all.
  size_t capacity = 16, used = 0, index = 0, position = 0;
  char * command_prefix = malloc(capacity);
  if (!command_prefix) {
    free(token_prefix);
    return UFCOMMAND_NO_MEMORY;
  }
  command_prefix[0] = '\0';
  while (position < start) {
    while (position < start && sIsSpace(line[position])) ++position;
    if (position >= start) break;
    size_t token_start = position;
    while (position < start && !sIsSpace(line[position])) ++position;
    size_t token_length = position - token_start;
    size_t needed       = used + (used ? 1 : 0) + token_length + 1;
    if (needed > capacity) {
      size_t grown_capacity = capacity;
      while (grown_capacity < needed) grown_capacity *= 2;
      char *grown = realloc(command_prefix, grown_capacity);
      if (!grown) {
        free(token_prefix);
        free(command_prefix);
        return UFCOMMAND_NO_MEMORY;
      }
      command_prefix = grown;
      capacity       = grown_capacity;
    }
    if (used) command_prefix[used++] = ' ';
    memcpy(command_prefix + used, line + token_start, token_length);
    used += token_length;
    command_prefix[used] = '\0';
    ++index;
  }

  query->line           = line;
  query->cursor         = cursor;
  query->token_prefix   = token_prefix;
  query->token_index    = index;
  query->command_prefix = command_prefix;
  *out_token_prefix     = token_prefix;
  *out_command_prefix   = command_prefix;
  *out_start            = start;
  *out_end              = end;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandCompletionCreate(UfCommandRegistry *dictionary, UfCommandCompletion **out_completion)
{
  if (!dictionary || !out_completion) return UFCOMMAND_INVALID_ARGUMENT;
  *out_completion = NULL;
  UfCommandCompletion *completion = calloc(1, sizeof(*completion));
  if (!completion) return UFCOMMAND_NO_MEMORY;
  completion->dictionary = dictionary;
  // Slot 0 is the dictionary, and it is not removable: a registry that opted in
  // always completes from its own commands and aliases.
  completion->providers[0].name      = "registry";
  completion->providers[0].provide   = sDictionaryProvider;
  completion->providers[0].user_data = dictionary;
  completion->provider_count         = 1;
  *out_completion                    = completion;
  return UFCOMMAND_OK;
}

void UfCommandCompletionDestroy(UfCommandCompletion *completion)
{
  free(completion);
}

UfCommandCompletion *UfCommandRegistryGetCompletion(UfCommandRegistry *registry)
{
  return registry ? registry->completion : NULL;
}

UfCommandStatus UfCommandCompletionAddProvider(UfCommandCompletion *                        completion,
                                               const UfCommandCompletionProviderDefinition *provider)
{
  if (!completion || !provider || !provider->provide || !provider->name || provider->name[0] == '\0')
    return UFCOMMAND_INVALID_ARGUMENT;
  // Refused while suggesting, so a provider cannot register code into the loop
  // that is running it.
  if (completion->suggest_depth != 0) return UFCOMMAND_CONFLICT;
  if (completion->provider_count >= UFCOMMAND_COMPLETION_MAX_PROVIDERS) return UFCOMMAND_BUFFER_TOO_SMALL;
  completion->providers[completion->provider_count++] = *provider;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandCompletionSuggest(UfCommandCompletion *completion, const char *line, size_t cursor,
                                           UfCommandCompletionResult **out_result)
{
  if (!completion || !line || !out_result) return UFCOMMAND_INVALID_ARGUMENT;
  *out_result = NULL;
  if (completion->suggest_depth != 0) return UFCOMMAND_CONFLICT;
  UfCommandCompletionResult *result = calloc(1, sizeof(*result));
  if (!result) return UFCOMMAND_NO_MEMORY;
  result->items = calloc(UFCOMMAND_COMPLETION_MAX_CANDIDATES, sizeof(*result->items));
  if (!result->items) {
    free(result);
    return UFCOMMAND_NO_MEMORY;
  }
  UfCommandCompletionQuery query;
  char *                   token_prefix = NULL, *command_prefix = NULL;
  size_t                   start = 0, end = 0;
  UfCommandStatus status = sBuildQuery(line, cursor, &query, &token_prefix, &command_prefix, &start, &end);
  if (status != UFCOMMAND_OK) {
    sFreeResult(result);
    return status;
  }
  result->replacement_start = start;
  result->replacement_end   = end;
  // Providers are host code reading the registry's private tables, so the
  // registry's mutation guard is raised for as long as they run.
  UfCommandRegistry *dictionary = completion->dictionary;
  completion->suggest_depth     = 1;
  dictionary->dispatch_depth++;
  UfCommandCompletionEmitContext emit_context = { result };
  size_t                         providers    = completion->provider_count;
  for (size_t i = 0; i < providers && status == UFCOMMAND_OK; ++i) {
    status = completion->providers[i].provide(completion->providers[i].user_data, &query, sEmit, &emit_context);
  }
  dictionary->dispatch_depth--;
  completion->suggest_depth = 0;
  free(token_prefix);
  free(command_prefix);
  if (status != UFCOMMAND_OK) {
    sFreeResult(result);
    return status;
  }
  // count > 1: qsort is given a NULL base otherwise, which the standard does not
  // permit even for a zero count.
  if (result->count > 1) qsort(result->items, result->count, sizeof(*result->items), sCandidateCompare);
  *out_result = result;
  return UFCOMMAND_OK;
}

void UfCommandCompletionResultDestroy(UfCommandCompletionResult *result)
{
  sFreeResult(result);
}

UfCommandStatus UfCommandCompletionResultGetInfo(const UfCommandCompletionResult *result,
                                                 UfCommandCompletionResultInfo *  out_info)
{
  if (!result || !out_info) return UFCOMMAND_INVALID_ARGUMENT;
  out_info->count             = result->count;
  out_info->replacement_start = result->replacement_start;
  out_info->replacement_end   = result->replacement_end;
  return UFCOMMAND_OK;
}

size_t UfCommandCompletionResultGetCount(const UfCommandCompletionResult *result)
{
  return result ? result->count : 0;
}

const UfCommandCompletionCandidate *UfCommandCompletionResultGetCandidate(const UfCommandCompletionResult *result,
                                                                         size_t index)
{
  if (!result || index >= result->count) return NULL;
  return &result->items[index];
}
