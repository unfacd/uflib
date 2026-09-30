#include <uflib/ufcommand/ufcommand.h>
#include "ufcommand_priv.h"
#include "ufcommand_completion_priv.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <ctype.h>

/* ── File-local helpers ──────────────────────────────────────────────────
 * These were a separate translation unit only by inheritance: the compiler
 * showed six of its eight functions were called from nowhere but this file,
 * so they are file-scope here.  UfCommandStrDup remains external because the
 * persistence layer uses it too. */

static size_t UfCommandHash(const char *s)
{
  uint64_t h = UINT64_C(1469598103934665603);
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= UINT64_C(1099511628211);
  }
  return (size_t)(h ^ (h >> 32));
}

static bool sIsSpace(unsigned char c) { return isspace(c) != 0; }

static void UfCommandFreeTokens(char **tokens, size_t count)
{
  if (!tokens) return;
  for (size_t i = 0; i < count; ++i) free(tokens[i]);
  free(tokens);
}

static UfCommandStatus UfCommandTokenize(const UfCommandParserConfig *cfg, const char *line, char ***out_tokens,
                                  size_t *                     out_count)
{
  if (!cfg || !line || !out_tokens || !out_count || cfg->max_tokens == 0 || cfg->max_token_length == 0) return
    UFCOMMAND_INVALID_ARGUMENT;
  if (cfg->max_tokens > SIZE_MAX / sizeof(char*)) return UFCOMMAND_INVALID_ARGUMENT;
  // The vector is fully written before it is read, so there is nothing to zero.
  char **tokens = malloc(cfg->max_tokens * sizeof(*tokens));
  if (!tokens) return UFCOMMAND_NO_MEMORY;
  size_t count = 0, i = 0;
  while (line[i] != '\0') {
    while (sIsSpace((unsigned char)line[i])) ++i;
    if (line[i] == '\0') break;
    if (count >= cfg->max_tokens) {
      UfCommandFreeTokens(tokens, count);
      return UFCOMMAND_PARSE_ERROR;
    }
    // A token body is accumulated on the stack and copied out at its exact
    // length afterwards.  Sizing every token to max_token_length() meant a
    // 6-byte command allocated 1025 bytes on every dispatch.
    char   scratch[UFCOMMAND_PRIV_TOKEN_INLINE];
    char * heap   = NULL, *tok = scratch;
    size_t n      = 0;
    bool   quoted = false;
    while (line[i] != '\0') {
      unsigned char c = (unsigned char)line[i];
      if (!quoted && sIsSpace(c)) break;
      if (c == (unsigned char)cfg->escape_char) {
        ++i;
        if (line[i] == '\0') {
          free(heap);
          UfCommandFreeTokens(tokens, count);
          return UFCOMMAND_PARSE_ERROR;
        }
        c = (unsigned char)line[i++];
      }
      else if (c == (unsigned char)cfg->quote_char) {
        quoted = !quoted;
        ++i;
        continue;
      }
      else {
        ++i;
      }
      if (n >= cfg->max_token_length) {
        free(heap);
        UfCommandFreeTokens(tokens, count);
        return UFCOMMAND_PARSE_ERROR;
      }
      if (heap == NULL && n == sizeof(scratch)) {
        heap = malloc(cfg->max_token_length + 1);
        if (!heap) {
          UfCommandFreeTokens(tokens, count);
          return UFCOMMAND_NO_MEMORY;
        }
        memcpy(heap, scratch, n);
        tok = heap;
      }
      tok[n++] = (char)c;
    }
    if (quoted) {
      free(heap);
      UfCommandFreeTokens(tokens, count);
      return UFCOMMAND_PARSE_ERROR;
    }
    if (heap != NULL) {
      heap[n]         = '\0';
      tokens[count++] = heap;
    }
    else {
      char *exact = malloc(n + 1);
      if (!exact) {
        UfCommandFreeTokens(tokens, count);
        return UFCOMMAND_NO_MEMORY;
      }
      memcpy(exact, scratch, n);
      exact[n]        = '\0';
      tokens[count++] = exact;
    }
  }
  *out_tokens = tokens;
  *out_count  = count;
  return UFCOMMAND_OK;
}

static UfCommandStatus UfCommandAppendResultArg(UfCommandResult *result, const char *value)
{
  if (!result || !value) return UFCOMMAND_INVALID_ARGUMENT;
  if (result->arg_count == result->arg_capacity) {
    size_t nc = result->arg_capacity ? result->arg_capacity * 2 : 8;
    if (nc < result->arg_capacity || nc > SIZE_MAX / sizeof(*result->args)) return UFCOMMAND_NO_MEMORY;
    UfCommandArg *na = malloc(nc * sizeof(*na));
    char **       no = malloc(nc * sizeof(*no));
    if (!na || !no) {
      free(na);
      free(no);
      return UFCOMMAND_NO_MEMORY;
    }
    if (result->arg_count) {
      memcpy(na, result->args, result->arg_count * sizeof(*na));
      memcpy(no, result->owned_args, result->arg_count * sizeof(*no));
    }
    free(result->args);
    free(result->owned_args);
    result->args         = na;
    result->owned_args   = no;
    result->arg_capacity = nc;
  }
  char *copy = UfCommandStrDup(value);
  if (!copy) return UFCOMMAND_NO_MEMORY;
  result->owned_args[result->arg_count]  = copy;
  result->args[result->arg_count].value  = copy;
  result->args[result->arg_count].length = strlen(copy);
  ++result->arg_count;
  return UFCOMMAND_OK;
}

static UfCommandStatus UfCommandSetResultCommand(UfCommandResult *result, const char *name)
{
  if (!result || !name) return UFCOMMAND_INVALID_ARGUMENT;
  char *p = UfCommandStrDup(name);
  if (!p) return UFCOMMAND_NO_MEMORY;
  free(result->resolved_command);
  result->resolved_command = p;
  return UFCOMMAND_OK;
}

void UfCommandRegistryDestroy(UfCommandRegistry *registry);
void UfCommandResultReset(UfCommandResult *result);

static size_t sNextPow2(size_t x)
{
  size_t p = 16;
  while (p < x && p <= SIZE_MAX / 2) p *= 2;
  return p;
}

static bool sNonEmpty(const char *s) { return s != NULL && *s != '\0'; }
static bool sValidSimpleName(const char *s) { return sNonEmpty(s) && strlen(s) < 4096; }

static size_t sHashBinding(UfCommandModifier modifiers, const char *key)
{
  uint64_t h = UINT64_C(1469598103934665603);
  h          ^= (unsigned char)'[';
  h          *= UINT64_C(1099511628211);
  for (size_t shift = 0; shift < sizeof(unsigned) * CHAR_BIT; shift += 8) {
    h ^= (unsigned char)(((unsigned)modifiers >> shift) & 0xffu);
    h *= UINT64_C(1099511628211);
  }
  h ^= (unsigned char)']';
  h *= UINT64_C(1099511628211);
  for (const unsigned char *p = (const unsigned char*)key; *p; ++p) {
    h ^= *p;
    h *= UINT64_C(1099511628211);
  }
  return (size_t)(h ^ (h >> 32));
}

static UfCommandEntryPrivate *sFindCommand(UfCommandRegistry *r, const char *name)
{
  if (!r || !name || !r->command_capacity) return NULL;
  size_t mask = r->command_capacity - 1;
  size_t h    = UfCommandHash(name), i = h & mask;
  for (;;) {
    UfCommandEntryPrivate *e = &r->commands[i];
    if (!e->occupied && !e->tombstone) return NULL;
    if (e->occupied && e->hash == h && strcmp(e->name, name) == 0) return e;
    i = (i + 1) & mask;
  }
}

static UfCommandAliasPrivate *sFindAlias(UfCommandRegistry *r, const char *name)
{
  if (!r || !name || !r->alias_capacity) return NULL;
  size_t mask = r->alias_capacity - 1;
  size_t h    = UfCommandHash(name), i = h & mask;
  for (;;) {
    UfCommandAliasPrivate *e = &r->aliases[i];
    if (!e->occupied && !e->tombstone) return NULL;
    if (e->occupied && e->hash == h && strcmp(e->name, name) == 0) return e;
    i = (i + 1) & mask;
  }
}

static UfCommandBindingPrivate *sFindBinding(UfCommandRegistry *r, UfCommandModifier m, const char *key)
{
  if (!r || !key || !r->binding_capacity) return NULL;
  size_t mask = r->binding_capacity - 1;
  size_t h    = sHashBinding(m, key), i = h & mask;
  for (;;) {
    UfCommandBindingPrivate *e = &r->bindings[i];
    if (!e->occupied && !e->tombstone) return NULL;
    if (e->occupied && e->hash == h && e->modifiers == m && strcmp(e->key, key) == 0) return e;
    i = (i + 1) & mask;
  }
}

static void sFreeCommandEntry(UfCommandEntryPrivate *e)
{
  free(e->name);
  free(e->description);
  memset(e, 0, sizeof(*e));
}

static void sFreeAliasEntry(UfCommandAliasPrivate *e)
{
  free(e->name);
  free(e->expansion);
  memset(e, 0, sizeof(*e));
}

static void sFreeBindingEntry(UfCommandBindingPrivate *e)
{
  free(e->key);
  free(e->command);
  memset(e, 0, sizeof(*e));
}

static UfCommandStatus sGrowCommands(UfCommandRegistry *r)
{
  if (r->command_capacity > SIZE_MAX / 2) return UFCOMMAND_NO_MEMORY;
  size_t                 nc = r->command_capacity * 2;
  UfCommandEntryPrivate *a  = calloc(nc, sizeof(*a));
  if (!a) return UFCOMMAND_NO_MEMORY;
  for (size_t j = 0; j < r->command_capacity; ++j)
    if (r->commands[j].occupied) {
      UfCommandEntryPrivate e = r->commands[j];
      size_t                i = e.hash & (nc - 1);
      while (a[i].occupied) i = (i + 1) & (nc - 1);
      a[i] = e;
    }
  free(r->commands);
  r->commands           = a;
  r->command_capacity   = nc;
  r->command_tombstones = 0;
  return UFCOMMAND_OK;
}

static UfCommandStatus sGrowAliases(UfCommandRegistry *r)
{
  if (r->alias_capacity > SIZE_MAX / 2) return UFCOMMAND_NO_MEMORY;
  size_t                 nc = r->alias_capacity * 2;
  UfCommandAliasPrivate *a  = calloc(nc, sizeof(*a));
  if (!a) return UFCOMMAND_NO_MEMORY;
  for (size_t j = 0; j < r->alias_capacity; ++j)
    if (r->aliases[j].occupied) {
      UfCommandAliasPrivate e = r->aliases[j];
      size_t                i = e.hash & (nc - 1);
      while (a[i].occupied) i = (i + 1) & (nc - 1);
      a[i] = e;
    }
  free(r->aliases);
  r->aliases          = a;
  r->alias_capacity   = nc;
  r->alias_tombstones = 0;
  return UFCOMMAND_OK;
}

static UfCommandStatus sGrowBindings(UfCommandRegistry *r)
{
  if (r->binding_capacity > SIZE_MAX / 2) return UFCOMMAND_NO_MEMORY;
  size_t                   nc = r->binding_capacity * 2;
  UfCommandBindingPrivate *a  = calloc(nc, sizeof(*a));
  if (!a) return UFCOMMAND_NO_MEMORY;
  for (size_t j = 0; j < r->binding_capacity; ++j)
    if (r->bindings[j].occupied) {
      UfCommandBindingPrivate e = r->bindings[j];
      size_t                  i = e.hash & (nc - 1);
      while (a[i].occupied) i = (i + 1) & (nc - 1);
      a[i] = e;
    }
  free(r->bindings);
  r->bindings           = a;
  r->binding_capacity   = nc;
  r->binding_tombstones = 0;
  return UFCOMMAND_OK;
}

static bool sCanonicalCommandName(const char *name, char **out, size_t *components)
{
  if (!sValidSimpleName(name) || !out || !components) return false;
  size_t len = strlen(name), n = 0, i = 0, outn = 0;
  char * buf = malloc(len + 1);
  if (!buf) return false;
  bool in = false;
  while (i < len) {
    while (i < len && isspace((unsigned char)name[i])) ++i;
    if (i == len) break;
    if (n && outn) buf[outn++] = ' ';
    while (i < len && !isspace((unsigned char)name[i])) { buf[outn++] = name[i++]; }
    ++n;
    in = true;
    (void)in;
  }
  if (n == 0) {
    free(buf);
    return false;
  }
  buf[outn]   = '\0';
  *out        = buf;
  *components = n;
  return true;
}

// UfCommandRegistryAdd() stores the canonical form, so every entry point that
// takes a caller-supplied name has to resolve it the same way.  Hashing the raw
// string let Add("status ") succeed while Find("status ") reported NOT_FOUND.
static UfCommandEntryPrivate *sFindCommandByName(UfCommandRegistry *r, const char *name)
{
  char * canonical  = NULL;
  size_t components = 0;
  if (!sCanonicalCommandName(name, &canonical, &components)) return NULL;
  UfCommandEntryPrivate *e = sFindCommand(r, canonical);
  free(canonical);
  return e;
}

UfCommandStatus UfCommandRegistryCreate(const UfCommandRegistryConfig *config, UfCommandRegistry **out)
{
  if (!out) return UFCOMMAND_INVALID_ARGUMENT;
  *out       = NULL;
  size_t cap = config && config->initial_capacity ? config->initial_capacity : 16;
  if (cap < 16) cap = 16;
  cap                  = sNextPow2(cap);
  UfCommandRegistry *r = calloc(1, sizeof(*r));
  if (!r) return UFCOMMAND_NO_MEMORY;
  r->command_capacity = r->alias_capacity = r->binding_capacity = cap;
  r->commands         = calloc(cap, sizeof(*r->commands));
  r->aliases          = calloc(cap, sizeof(*r->aliases));
  r->bindings         = calloc(cap, sizeof(*r->bindings));
  if (!r->commands || !r->aliases || !r->bindings) {
    UfCommandRegistryDestroy(r);
    return UFCOMMAND_NO_MEMORY;
  }
  r->key_config.default_modifiers = UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT;
  // Instrumentation is opt-in: it costs roughly a quarter of a multi-prefix
  // dispatch, and a host that never dumps the record should not pay for it.
  if (config && config->telemetry_enabled) {
    if (UfCommandTelemetryCreate(&r->telemetry) != UFCOMMAND_OK) {
      UfCommandRegistryDestroy(r);
      return UFCOMMAND_NO_MEMORY;
    }
    r->telemetry_owned = true;
  }
  // Also opt-in, and for the same reason: a registry that is never asked to
  // suggest should not carry an engine or pay for the lookup it does.
  if (config && config->completion_enabled) {
    if (UfCommandCompletionCreate(r, &r->completion) != UFCOMMAND_OK) {
      UfCommandRegistryDestroy(r);
      return UFCOMMAND_NO_MEMORY;
    }
  }
  // Borrowed, optional: NULL means the module reports failures through status
  // codes alone, which is the default.
  r->uf_logger = config ? config->uf_logger : NULL;
  UFCOMMAND_LOG_INFO(r, "registry created: capacity %zu, telemetry %s, completion %s", r->command_capacity,
                     r->telemetry ? "on" : "off", r->completion ? "on" : "off");
  r->max_command_components = config && config->max_command_components ? config->max_command_components : SIZE_MAX;
  *out                      = r;
  return UFCOMMAND_OK;
}

void UfCommandRegistryDestroy(UfCommandRegistry *r)
{
  if (!r) return;
  for (size_t i = 0; i < r->command_capacity; ++i) sFreeCommandEntry(&r->commands[i]);
  for (size_t i = 0; i < r->alias_capacity; ++i) sFreeAliasEntry(&r->aliases[i]);
  for (size_t i = 0; i < r->binding_capacity; ++i) sFreeBindingEntry(&r->bindings[i]);
  UFCOMMAND_LOG_INFO(r, "registry destroyed: %zu command(s), %zu alias(es), %zu binding(s)", r->command_count,
                     r->alias_count, r->binding_count);
  if (r->telemetry_owned) UfCommandTelemetryDestroy(r->telemetry);
  if (r->completion) UfCommandCompletionDestroy(r->completion);
  free(r->commands);
  free(r->aliases);
  free(r->bindings);
  free(r);
}

UfCommandStatus UfCommandRegistryAdd(UfCommandRegistry *r, const UfCommandDefinition *d)
{
  if (!r || !d || !sNonEmpty(d->name) || !d->handler || d->min_args > d->max_args) return UFCOMMAND_INVALID_ARGUMENT;
  if (r->dispatch_depth != 0) return UFCOMMAND_CONFLICT;
  char * canonical  = NULL;
  size_t components = 0;
  if (!sCanonicalCommandName(d->name, &canonical, &components)) return UFCOMMAND_INVALID_COMMAND;
  if (components > r->max_command_components) {
    free(canonical);
    return UFCOMMAND_INVALID_COMMAND;
  }
  if (sFindCommand(r, canonical)) {
    UFCOMMAND_LOG_WARN(r, "add: command '%s' is already registered", canonical);
    free(canonical);
    return UFCOMMAND_DUPLICATE;
  }
  if (sFindAlias(r, canonical)) {
    UFCOMMAND_LOG_WARN(r, "add: '%s' is an alias, not a command", canonical);
    free(canonical);
    return UFCOMMAND_CONFLICT;
  }
  if ((r->command_count + r->command_tombstones + 1) * 10 >= r->command_capacity * 7) {
    UfCommandStatus st = sGrowCommands(r);
    if (st != UFCOMMAND_OK) {
      free(canonical);
      return st;
    }
  }
  size_t hash = UfCommandHash(canonical);
  size_t i    = hash & (r->command_capacity - 1), tomb = SIZE_MAX;
  while (r->commands[i].occupied || r->commands[i].tombstone) {
    if (r->commands[i].tombstone && tomb == SIZE_MAX) tomb = i;
    i = (i + 1) & (r->command_capacity - 1);
  }
  if (tomb != SIZE_MAX) i = tomb;
  UfCommandEntryPrivate *e = &r->commands[i];
  if (e->tombstone) {
    e->tombstone = false;
    --r->command_tombstones;
  }
  e->name        = canonical;
  e->description = UfCommandStrDup(d->description ? d->description : "");
  e->hash        = hash;
  if (!e->description) {
    free(e->name);
    memset(e, 0, sizeof(*e));
    return UFCOMMAND_NO_MEMORY;
  }
  e->handler   = d->handler;
  e->user_data = d->user_data;
  e->min_args  = d->min_args;
  e->max_args  = d->max_args;
  e->occupied  = true;
  ++r->command_count;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandRegistryRemove(UfCommandRegistry *r, const char *name)
{
  if (!r || !sNonEmpty(name)) return UFCOMMAND_INVALID_ARGUMENT;
  if (r->dispatch_depth != 0) return UFCOMMAND_CONFLICT;
  UfCommandEntryPrivate *e = sFindCommandByName(r, name);
  if (!e) {
    UFCOMMAND_LOG_WARN(r, "remove: no command '%s'", name);
    return UFCOMMAND_NOT_FOUND;
  }
  sFreeCommandEntry(e);
  e->tombstone = true;
  --r->command_count;
  ++r->command_tombstones;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandRegistryFind(const UfCommandRegistry *r, const char *name, UfCommandDefinition *out)
{
  if (!r || !sNonEmpty(name) || !out) return UFCOMMAND_INVALID_ARGUMENT;
  UfCommandEntryPrivate *e = sFindCommandByName((UfCommandRegistry*)r, name);
  if (!e) {
    UFCOMMAND_LOG_WARN(r, "find: no command '%s'", name);
    return UFCOMMAND_NOT_FOUND;
  }
  out->name        = e->name;
  out->description = e->description;
  out->handler     = e->handler;
  out->user_data   = e->user_data;
  out->min_args    = e->min_args;
  out->max_args    = e->max_args;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandRegistryAddAlias(UfCommandRegistry *r, const UfCommandAliasDefinition *d)
{
  if (!r || !d || !sValidSimpleName(d->alias) || !sNonEmpty(d->expansion)) return UFCOMMAND_INVALID_ALIAS;
  if (r->dispatch_depth != 0) return UFCOMMAND_CONFLICT;
  if (strchr(d->alias, ' ') != NULL || strchr(d->alias, '\t') != NULL) return UFCOMMAND_INVALID_ALIAS;
  if (sFindAlias(r, d->alias)) {
    UFCOMMAND_LOG_WARN(r, "add alias: '%s' is already an alias", d->alias);
    return UFCOMMAND_DUPLICATE;
  }
  if (sFindCommand(r, d->alias)) {
    UFCOMMAND_LOG_WARN(r, "add alias: '%s' is a registered command", d->alias);
    return UFCOMMAND_CONFLICT;
  }
  if ((r->alias_count + r->alias_tombstones + 1) * 10 >= r->alias_capacity * 7) {
    UfCommandStatus st = sGrowAliases(r);
    if (st != UFCOMMAND_OK) return st;
  }
  size_t hash = UfCommandHash(d->alias);
  size_t i    = hash & (r->alias_capacity - 1), tomb = SIZE_MAX;
  while (r->aliases[i].occupied || r->aliases[i].tombstone) {
    if (r->aliases[i].tombstone && tomb == SIZE_MAX) tomb = i;
    i = (i + 1) & (r->alias_capacity - 1);
  }
  if (tomb != SIZE_MAX) i = tomb;
  UfCommandAliasPrivate *e = &r->aliases[i];
  if (e->tombstone) {
    e->tombstone = false;
    --r->alias_tombstones;
  }
  e->name      = UfCommandStrDup(d->alias);
  e->expansion = UfCommandStrDup(d->expansion);
  e->hash      = hash;
  if (!e->name || !e->expansion) {
    free(e->name);
    free(e->expansion);
    memset(e, 0, sizeof(*e));
    return UFCOMMAND_NO_MEMORY;
  }
  e->occupied = true;
  ++r->alias_count;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandRegistryRemoveAlias(UfCommandRegistry *r, const char *alias)
{
  if (!r || !sNonEmpty(alias)) return UFCOMMAND_INVALID_ARGUMENT;
  if (r->dispatch_depth != 0) return UFCOMMAND_CONFLICT;
  UfCommandAliasPrivate *e = sFindAlias(r, alias);
  if (!e) return UFCOMMAND_NOT_FOUND;
  sFreeAliasEntry(e);
  e->tombstone = true;
  --r->alias_count;
  ++r->alias_tombstones;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandRegistryAddBinding(UfCommandRegistry *r, const UfCommandBinding *b)
{
  if (!r || !b || !sNonEmpty(b->key) || !sNonEmpty(b->command)) return UFCOMMAND_INVALID_ARGUMENT;
  if (r->dispatch_depth != 0) return UFCOMMAND_CONFLICT;
  if ((unsigned)b->modifiers & ~((unsigned)UFCOMMAND_MOD_CTRL | (unsigned)UFCOMMAND_MOD_SHIFT | (unsigned)
    UFCOMMAND_MOD_ALT | (unsigned)UFCOMMAND_MOD_META)) return UFCOMMAND_INVALID_ARGUMENT;
  if (sFindBinding(r, b->modifiers, b->key)) {
    UFCOMMAND_LOG_WARN(r, "add binding: duplicate chord %u:%s", (unsigned)b->modifiers, b->key);
    return UFCOMMAND_DUPLICATE;
  }
  if ((r->binding_count + r->binding_tombstones + 1) * 10 >= r->binding_capacity * 7) {
    UfCommandStatus st = sGrowBindings(r);
    if (st != UFCOMMAND_OK) return st;
  }
  size_t hash = sHashBinding(b->modifiers, b->key);
  size_t i    = hash & (r->binding_capacity - 1), tomb = SIZE_MAX;
  while (r->bindings[i].occupied || r->bindings[i].tombstone) {
    if (r->bindings[i].tombstone && tomb == SIZE_MAX) tomb = i;
    i = (i + 1) & (r->binding_capacity - 1);
  }
  if (tomb != SIZE_MAX) i = tomb;
  UfCommandBindingPrivate *e = &r->bindings[i];
  if (e->tombstone) {
    e->tombstone = false;
    --r->binding_tombstones;
  }
  e->key     = UfCommandStrDup(b->key);
  e->command = UfCommandStrDup(b->command);
  e->hash    = hash;
  if (!e->key || !e->command) {
    free(e->key);
    free(e->command);
    memset(e, 0, sizeof(*e));
    return UFCOMMAND_NO_MEMORY;
  }
  e->modifiers = b->modifiers;
  e->occupied  = true;
  ++r->binding_count;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandRegistryRemoveBinding(UfCommandRegistry *r, UfCommandModifier m, const char *key)
{
  if (!r || !sNonEmpty(key)) return UFCOMMAND_INVALID_ARGUMENT;
  if (r->dispatch_depth != 0) return UFCOMMAND_CONFLICT;
  UfCommandBindingPrivate *e = sFindBinding(r, m, key);
  if (!e) return UFCOMMAND_NOT_FOUND;
  sFreeBindingEntry(e);
  e->tombstone = true;
  --r->binding_count;
  ++r->binding_tombstones;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandRegistrySetKeyConfig(UfCommandRegistry *r, const UfCommandKeyConfig *c)
{
  if (!r || !c) return UFCOMMAND_INVALID_ARGUMENT;
  if (r->dispatch_depth != 0) return UFCOMMAND_CONFLICT;
  if ((unsigned)c->default_modifiers & ~((unsigned)UFCOMMAND_MOD_CTRL | (unsigned)UFCOMMAND_MOD_SHIFT | (unsigned)
    UFCOMMAND_MOD_ALT | (unsigned)UFCOMMAND_MOD_META)) return UFCOMMAND_INVALID_ARGUMENT;
  r->key_config = *c;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandRegistryGetKeyConfig(const UfCommandRegistry *r, UfCommandKeyConfig *out)
{
  if (!r || !out) return UFCOMMAND_INVALID_ARGUMENT;
  *out = r->key_config;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandContextCreate(const UfCommandContextConfig *config, UfCommandContext **out)
{
  if (!out) return UFCOMMAND_INVALID_ARGUMENT;
  *out = calloc(1, sizeof(**out));
  if (!*out) return UFCOMMAND_NO_MEMORY;
  if (config) (*out)->user_data = config->user_data;
  return UFCOMMAND_OK;
}

void UfCommandContextDestroy(UfCommandContext *c) { free(c); }

UfCommandStatus UfCommandContextSetUserData(UfCommandContext *c, void *u)
{
  if (!c) return UFCOMMAND_INVALID_ARGUMENT;
  c->user_data = u;
  return UFCOMMAND_OK;
}

void *UfCommandContextGetUserData(const UfCommandContext *c) { return c ? c->user_data : NULL; }

UfCommandStatus UfCommandParserCreate(const UfCommandParserConfig *c, UfCommandParser **out)
{
  if (!out) return UFCOMMAND_INVALID_ARGUMENT;
  *out               = NULL;
  UfCommandParser *p = calloc(1, sizeof(*p));
  if (!p) return UFCOMMAND_NO_MEMORY;
  p->config = (UfCommandParserConfig){64, 1024, '"', '\\', 32};
  if (c) {
    if (!c->max_tokens || !c->max_token_length || !c->max_alias_depth) {
      free(p);
      return UFCOMMAND_INVALID_ARGUMENT;
    }
    p->config = *c;
  }
  *out = p;
  return UFCOMMAND_OK;
}

void UfCommandParserDestroy(UfCommandParser *p) { free(p); }

static bool sPlaceholder(const char *s, size_t *pos, bool *all)
{
  *all = false;
  *pos = 0;
  if (!s || s[0] != '$') return false;
  if (s[1] == '*' && s[2] == '\0') {
    *all = true;
    return true;
  }
  if (s[1] < '1' || s[1] > '9' || s[2] != '\0') return false;
  *pos = (size_t)(s[1] - '0');
  return true;
}

static UfCommandStatus sExpandAliasOnce(const UfCommandAliasPrivate *a, char **  tokens, size_t count,
                                        const UfCommandParserConfig *pc, char ***out, size_t *  out_count)
{
  char **         exp = NULL;
  size_t          ec  = 0;
  UfCommandStatus st  = UfCommandTokenize(pc, a->expansion, &exp, &ec);
  if (st != UFCOMMAND_OK) return st;
  size_t cap = pc->max_tokens;
  char **nt  = calloc(cap, sizeof(*nt));
  if (!nt) {
    UfCommandFreeTokens(exp, ec);
    return UFCOMMAND_NO_MEMORY;
  }
  size_t n = 0;
  for (size_t i = 0; i < ec; ++i) {
    size_t pos = 0;
    bool   all = false;
    if (sPlaceholder(exp[i], &pos, &all)) {
      if (all) {
        for (size_t k = 1; k < count; ++k) {
          if (n >= cap) {
            UfCommandFreeTokens(exp, ec);
            UfCommandFreeTokens(nt, n);
            return UFCOMMAND_PARSE_ERROR;
          }
          nt[n] = UfCommandStrDup(tokens[k]);
          if (!nt[n]) {
            UfCommandFreeTokens(exp, ec);
            UfCommandFreeTokens(nt, n);
            return UFCOMMAND_NO_MEMORY;
          }
          ++n;
        }
      }
      else {
        if (pos >= count || n >= cap) {
          UfCommandFreeTokens(exp, ec);
          UfCommandFreeTokens(nt, n);
          return UFCOMMAND_PARSE_ERROR;
        }
        nt[n] = UfCommandStrDup(tokens[pos]);
        if (!nt[n]) {
          UfCommandFreeTokens(exp, ec);
          UfCommandFreeTokens(nt, n);
          return UFCOMMAND_NO_MEMORY;
        }
        ++n;
      }
    }
    else {
      if (n >= cap) {
        UfCommandFreeTokens(exp, ec);
        UfCommandFreeTokens(nt, n);
        return UFCOMMAND_PARSE_ERROR;
      }
      nt[n] = UfCommandStrDup(exp[i]);
      if (!nt[n]) {
        UfCommandFreeTokens(exp, ec);
        UfCommandFreeTokens(nt, n);
        return UFCOMMAND_NO_MEMORY;
      }
      ++n;
    }
  }
  UfCommandFreeTokens(exp, ec);
  *out       = nt;
  *out_count = n;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandResolveAlias(UfCommandRegistry *          r, char ***tokens_io, size_t *count_io,
                                      const UfCommandParserConfig *pc)
{
  if (!r || !tokens_io || !count_io || !*tokens_io || !pc) return UFCOMMAND_INVALID_ARGUMENT;
  char **tokens = *tokens_io;
  size_t count  = *count_io;
  char **seen   = NULL;
  for (size_t depth = 0; depth < pc->max_alias_depth; ++depth) {
    UfCommandAliasPrivate *a = sFindAlias(r, tokens[0]);
    if (!a) {
      if (seen) {
        for (size_t j = 0; j < depth; ++j) free(seen[j]);
        free(seen);
      }
      *tokens_io = tokens;
      *count_io  = count;
      return UFCOMMAND_OK;
    }
    if (!seen) {
      seen = calloc(pc->max_alias_depth, sizeof(*seen));
      if (!seen) return UFCOMMAND_NO_MEMORY;
    }
    // Advisory: the status is discarded so instrumentation cannot change dispatch.
    if (r->telemetry) (void)UfCommandTelemetryAlias(r->telemetry, tokens[0]);
    for (size_t j = 0; j < depth; ++j) if (strcmp(tokens[0], seen[j]) == 0) {
      for (size_t j2 = 0; j2 < depth; ++j2) free(seen[j2]);
      free(seen);
      UFCOMMAND_LOG_WARN(r, "alias '%s' refers back to itself", tokens[0]);
      *tokens_io = tokens;
      *count_io  = count;
      return UFCOMMAND_ALIAS_CYCLE;
    }
    seen[depth] = UfCommandStrDup(tokens[0]);
    if (!seen[depth]) {
      for (size_t j = 0; j < depth; ++j) free(seen[j]);
      free(seen);
      return UFCOMMAND_NO_MEMORY;
    }
    char **         next       = NULL;
    size_t          next_count = 0;
    UfCommandStatus st         = sExpandAliasOnce(a, tokens, count, pc, &next, &next_count);
    if (st != UFCOMMAND_OK) {
      for (size_t j = 0; j <= depth; ++j) free(seen[j]);
      free(seen);
      return st;
    }
    UfCommandFreeTokens(tokens, count);
    tokens     = next;
    count      = next_count;
    // Publish as soon as the live vector changes.  sExpandAliasOnce() failure and
    // the empty-vector case both return below, and the caller frees what
    // *tokens_io names: leaving it pointing at the vector just released made
    // every second-round expansion failure a double free.
    *tokens_io = tokens;
    *count_io  = count;
    if (count == 0) {
      for (size_t j = 0; j <= depth; ++j) free(seen[j]);
      free(seen);
      return UFCOMMAND_PARSE_ERROR;
    }
  }
  if (seen) {
    for (size_t j = 0; j < pc->max_alias_depth; ++j) free(seen[j]);
    free(seen);
  }
  *tokens_io = tokens;
  *count_io  = count;
  UFCOMMAND_LOG_WARN(r, "alias chain exceeded the configured depth of %zu", pc->max_alias_depth);
  return UFCOMMAND_ALIAS_DEPTH_EXCEEDED;
}

static UfCommandEntryPrivate *sFindCommandPrefix(UfCommandRegistry *r, char *const *tokens, size_t count,
                                                 size_t *           consumed)
{
  for (size_t n = count; n > 0; --n) {
    size_t total = 0;
    for (size_t i = 0; i < n; ++i) {
      size_t len = strlen(tokens[i]);
      if (total > SIZE_MAX - len - (i ? 1 : 0)) return NULL;
      total += len + (i ? 1 : 0);
    }
    char *candidate = malloc(total + 1);
    if (!candidate) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
      if (i) candidate[pos++] = ' ';
      size_t len = strlen(tokens[i]);
      memcpy(candidate + pos, tokens[i], len);
      pos += len;
    }
    candidate[pos]           = '\0';
    UfCommandEntryPrivate *e = sFindCommand(r, candidate);
    free(candidate);
    if (e) {
      *consumed = n;
      return e;
    }
  }
  return NULL;
}

UfCommandStatus UfCommandExecuteTokens(UfCommandRegistry *r, UfCommandContext *c, char **tokens, size_t count,
                                       UfCommandResult *  res)
{
  if (!r || !tokens || !count || !res) return UFCOMMAND_INVALID_ARGUMENT;
  // Every failure below is the dispatcher's own, and each must be recorded as
  // such: only the handler's status may travel back through handler_status.
  size_t                 consumed = 0;
  UfCommandEntryPrivate *e        = sFindCommandPrefix(r, tokens, count, &consumed);
  if (!e) {
    UFCOMMAND_LOG_WARN(r, "no command matches '%s'", tokens[0]);
    res->dispatch_status = UFCOMMAND_NOT_FOUND;
    return UFCOMMAND_NOT_FOUND;
  }
  size_t argc = count - consumed;
  if (argc < e->min_args || argc > e->max_args) {
    UFCOMMAND_LOG_WARN(r, "'%s' takes %zu..%zu argument(s), got %zu", e->name, e->min_args, e->max_args, argc);
    res->dispatch_status = UFCOMMAND_ARGUMENT_COUNT;
    return UFCOMMAND_ARGUMENT_COUNT;
  }
  // Count every registered prefix, not just the match: 'remote add x y' increments
  // both 'remote' and 'remote add' when both are registered.  The prefixes the
  // dispatcher tried on the way are not retained, so they are re-derived here --
  // and only when an observer is attached.
  if (r->telemetry) {
    for (size_t n = 1; n <= consumed; ++n) {
      size_t total = 0;
      for (size_t i = 0; i < n; ++i) total += strlen(tokens[i]) + (i ? 1U : 0U);
      char *prefix = malloc(total + 1U);
      if (!prefix) break;
      size_t pos = 0;
      for (size_t i = 0; i < n; ++i) {
        if (i) prefix[pos++] = ' ';
        size_t len = strlen(tokens[i]);
        memcpy(prefix + pos, tokens[i], len);
        pos += len;
      }
      prefix[pos] = '\0';
      if (sFindCommand(r, prefix)) (void)UfCommandTelemetryCommand(r->telemetry, prefix);
      free(prefix);
    }
  }
  UfCommandStatus st = UfCommandSetResultCommand(res, e->name);
  if (st != UFCOMMAND_OK) {
    res->dispatch_status = st;
    return st;
  }
  for (size_t i = consumed; i < count; ++i) {
    st = UfCommandAppendResultArg(res, tokens[i]);
    if (st != UFCOMMAND_OK) {
      res->dispatch_status = st;
      return st;
    }
  }
  res->dispatch_status = UFCOMMAND_OK;
  ++r->dispatch_depth;
  res->handler_status = e->handler(c, e->name, res->args, res->arg_count, e->user_data);
  --r->dispatch_depth;
  return res->handler_status;
}

UfCommandStatus UfCommandParserExecute(UfCommandParser *p, UfCommandRegistry *r, UfCommandContext *c, const char *line,
                                       UfCommandResult *res)
{
  if (!p || !r || !line || !res) {
    if (res) res->dispatch_status = UFCOMMAND_INVALID_ARGUMENT;
    return UFCOMMAND_INVALID_ARGUMENT;
  }
  UfCommandResultReset(res);
  char **         t  = NULL;
  size_t          n  = 0;
  UfCommandStatus st = UfCommandTokenize(&p->config, line, &t, &n);
  if (st != UFCOMMAND_OK) {
    res->dispatch_status = st;
    return st;
  }
  if (n == 0) {
    UfCommandFreeTokens(t, n);
    res->dispatch_status = UFCOMMAND_PARSE_ERROR;
    return UFCOMMAND_PARSE_ERROR;
  }
  st = UfCommandResolveAlias(r, &t, &n, &p->config);
  if (st == UFCOMMAND_OK) st = UfCommandExecuteTokens(r, c, t, n, res);
  UfCommandFreeTokens(t, n);
  // ExecuteTokens records its own failures and the handler's separately, so an
  // unrecorded non-OK status here can only be the handler's own return.
  if (st != UFCOMMAND_OK && res->dispatch_status == UFCOMMAND_OK && res->handler_status == UFCOMMAND_OK) res->
    dispatch_status = st;
  return st;
}

UfCommandStatus UfCommandParserExecuteBinding(UfCommandParser * p, UfCommandRegistry *r, UfCommandContext * c,
                                              UfCommandModifier m, const char *       key, UfCommandResult *res)
{
  if (!p || !r || !sNonEmpty(key) || !res) {
    if (res) res->dispatch_status = UFCOMMAND_INVALID_ARGUMENT;
    return UFCOMMAND_INVALID_ARGUMENT;
  }
  UfCommandResultReset(res);
  UfCommandBindingPrivate *b = sFindBinding(r, m, key);
  if (!b) {
    UFCOMMAND_LOG_WARN(r, "no binding for chord %u:%s", (unsigned)m, key);
    res->dispatch_status = UFCOMMAND_NO_BINDING;
    return UFCOMMAND_NO_BINDING;
  }
  if (r->telemetry) (void)UfCommandTelemetryBinding(r->telemetry, m, key);
  return UfCommandParserExecute(p, r, c, b->command, res);
}

UfCommandStatus UfCommandParserExecuteDefaultBinding(UfCommandParser *p, UfCommandRegistry *r, UfCommandContext *c,
                                                     const char *     key, UfCommandResult *res)
{
  if (!r) {
    if (res)res->dispatch_status = UFCOMMAND_INVALID_ARGUMENT;
    return UFCOMMAND_INVALID_ARGUMENT;
  }
  return UfCommandParserExecuteBinding(p, r, c, r->key_config.default_modifiers, key, res);
}

UfCommandStatus UfCommandResultCreate(UfCommandResult **out)
{
  if (!out)return UFCOMMAND_INVALID_ARGUMENT;
  *out = calloc(1, sizeof(**out));
  if (!*out)return UFCOMMAND_NO_MEMORY;
  return UFCOMMAND_OK;
}

void UfCommandResultReset(UfCommandResult *r)
{
  if (!r)return;
  free(r->resolved_command);
  for (size_t i = 0; i < r->arg_count; ++i)free(r->owned_args[i]);
  free(r->args);
  free(r->owned_args);
  memset(r, 0, sizeof(*r));
  r->dispatch_status = UFCOMMAND_OK;
  r->handler_status  = UFCOMMAND_OK;
}

void UfCommandResultDestroy(UfCommandResult *r)
{
  if (!r)return;
  UfCommandResultReset(r);
  free(r);
}

UfCommandStatus UfCommandResultGetStatus(const UfCommandResult *r)
{
  if (!r)return UFCOMMAND_INVALID_ARGUMENT;
  return r->dispatch_status != UFCOMMAND_OK ? r->dispatch_status : r->handler_status;
}

UfCommandStatus UfCommandResultGetDispatchStatus(const UfCommandResult *r)
{
  return r ? r->dispatch_status : UFCOMMAND_INVALID_ARGUMENT;
}

UfCommandStatus UfCommandResultGetHandlerStatus(const UfCommandResult *r)
{
  return r ? r->handler_status : UFCOMMAND_INVALID_ARGUMENT;
}

UfCommandStatus UfCommandResultGetInfo(const UfCommandResult *r, UfCommandResultInfo *out)
{
  if (!r || !out)return UFCOMMAND_INVALID_ARGUMENT;
  out->dispatch_status = r->dispatch_status;
  out->handler_status  = r->handler_status;
  return UFCOMMAND_OK;
}

const char *UfCommandResultGetResolvedCommand(const UfCommandResult *r) { return r ? r->resolved_command : NULL; }
size_t UfCommandResultGetArgCount(const UfCommandResult *r) { return r ? r->arg_count : 0; }
const UfCommandArg *UfCommandResultGetArgs(const UfCommandResult *r) { return r ? r->args : NULL; }

const char *UfCommandStatusName(UfCommandStatus s)
{
  switch (s) {
  case UFCOMMAND_OK: return "OK";
  case UFCOMMAND_NOT_FOUND: return "NOT_FOUND";
  case UFCOMMAND_PARSE_ERROR: return "PARSE_ERROR";
  case UFCOMMAND_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
  case UFCOMMAND_ARGUMENT_COUNT: return "ARGUMENT_COUNT";
  case UFCOMMAND_DUPLICATE: return "DUPLICATE";
  case UFCOMMAND_NO_MEMORY: return "NO_MEMORY";
  case UFCOMMAND_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
  case UFCOMMAND_HANDLER_ERROR: return "HANDLER_ERROR";
  case UFCOMMAND_ALIAS_CYCLE: return "ALIAS_CYCLE";
  case UFCOMMAND_ALIAS_DEPTH_EXCEEDED: return "ALIAS_DEPTH_EXCEEDED";
  case UFCOMMAND_CONFLICT: return "CONFLICT";
  case UFCOMMAND_IO_ERROR: return "IO_ERROR";
  case UFCOMMAND_CONFIG_ERROR: return "CONFIG_ERROR";
  case UFCOMMAND_UNSUPPORTED_VERSION: return "UNSUPPORTED_VERSION";
  case UFCOMMAND_INVALID_COMMAND: return "INVALID_COMMAND";
  case UFCOMMAND_INVALID_ALIAS: return "INVALID_ALIAS";
  case UFCOMMAND_NO_BINDING: return "NO_BINDING";
  case UFCOMMAND_CANCELLED: return "CANCELLED";
  default: return "UNKNOWN";
  }
}
