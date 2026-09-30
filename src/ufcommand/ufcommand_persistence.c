#include "ufcommand_priv.h"
#include "ufcommand_completion_priv.h"
#include <uflib/ufcommand/ufcommand.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* The persisted format deliberately uses a small Lua-table subset.  It is
 * valid Lua source, but the library does not embed a Lua interpreter: loading
 * is deterministic and does not execute arbitrary code. */

typedef enum
{
  TOK_EOF = 0, TOK_IDENT, TOK_STRING, TOK_NUMBER,
  TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
  TOK_EQUAL, TOK_COMMA
} TokenKind;

typedef struct
{
  TokenKind kind;
  char *    text;
  size_t    line;
} Token;

typedef struct
{
  const char *src;
  size_t      pos;
  size_t      line;
  Token       current;
} Lexer;

typedef struct
{
  char *alias;
  char *expansion;
} AliasConfig;

typedef struct
{
  UfCommandModifier modifiers;
  char *            key;
  char *            command;
} BindingConfig;


typedef struct
{
  AliasConfig *      aliases;
  size_t             alias_count;
  BindingConfig *    bindings;
  size_t             binding_count;
  UfCommandKeyConfig key_config;
  bool               key_config_seen;
  unsigned           version;
  bool               version_seen;
} LoadedConfig;

static void sTokenFree(Token *t)
{
  free(t->text);
  t->text = NULL;
}

static bool sIsIdentStart(int c) { return isalpha((unsigned char)c) || c == '_'; }
static bool sIsIdentPart(int c) { return isalnum((unsigned char)c) || c == '_'; }

static char *sDupRange(const char *s, size_t n)
{
  char *p = malloc(n + 1);
  if (p) {
    memcpy(p, s, n);
    p[n] = '\0';
  }
  return p;
}

static int sHex(int c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static char *sReadLuaString(Lexer *lx, char quote)
{
  size_t cap = 32, n = 0;
  char * out = malloc(cap);
  if (!out) return NULL;
  while (lx->src[lx->pos] != '\0') {
    int c = (unsigned char)lx->src[lx->pos++];
    if (c == quote) {
      out[n] = '\0';
      return out;
    }
    if (c == '\n') lx->line++;
    if (c == '\\') {
      if (lx->src[lx->pos] == '\0') {
        free(out);
        return NULL;
      }
      c = (unsigned char)lx->src[lx->pos++];
      if (c == 'x') {
        if (lx->src[lx->pos] == '\0' || lx->src[lx->pos + 1] == '\0') {
          free(out);
          return NULL;
        }
      }
      switch (c) {
      case 'n': c = '\n';
        break;
      case 'r': c = '\r';
        break;
      case 't': c = '\t';
        break;
      case '\\':
      case '\"':
      case '\'': break;
      case 'x': {
        int a = sHex((unsigned char)lx->src[lx->pos++]);
        int b = sHex((unsigned char)lx->src[lx->pos++]);
        if (a < 0 || b < 0) {
          free(out);
          return NULL;
        }
        c = (a << 4) | b;
        break;
      }
      default: free(out);
        return NULL;
      }
    }
    if (n + 1 >= cap) {
      size_t nc = cap * 2;
      char * p  = realloc(out, nc);
      if (!p) {
        free(out);
        return NULL;
      }
      out = p;
      cap = nc;
    }
    out[n++] = (char)c;
  }
  free(out);
  return NULL;
}

static bool sNextToken(Lexer *lx)
{
  sTokenFree(&lx->current);
  while (1) {
    int c = (unsigned char)lx->src[lx->pos];
    if (c == '\0') {
      lx->current.kind = TOK_EOF;
      lx->current.line = lx->line;
      return true;
    }
    if (isspace((unsigned char)c)) {
      if (c == '\n') lx->line++;
      lx->pos++;
      continue;
    }
    if (c == '-' && lx->src[lx->pos + 1] == '-') {
      lx->pos += 2;
      while (lx->src[lx->pos] && lx->src[lx->pos] != '\n') lx->pos++;
      continue;
    }
    lx->current.line = lx->line;
    switch (c) {
    case '{': lx->pos++;
      lx->current.kind = TOK_LBRACE;
      return true;
    case '}': lx->pos++;
      lx->current.kind = TOK_RBRACE;
      return true;
    case '[': lx->pos++;
      lx->current.kind = TOK_LBRACKET;
      return true;
    case ']': lx->pos++;
      lx->current.kind = TOK_RBRACKET;
      return true;
    case '=': lx->pos++;
      lx->current.kind = TOK_EQUAL;
      return true;
    case ',': lx->pos++;
      lx->current.kind = TOK_COMMA;
      return true;
    case '\"':
    case '\'':
      lx->pos++;
      lx->current.text = sReadLuaString(lx, (char)c);
      if (!lx->current.text) return false;
      lx->current.kind = TOK_STRING;
      return true;
    default: break;
    }
    if (sIsIdentStart(c)) {
      size_t start = lx->pos++;
      while (sIsIdentPart((unsigned char)lx->src[lx->pos])) lx->pos++;
      lx->current.text = sDupRange(lx->src + start, lx->pos - start);
      if (!lx->current.text) return false;
      lx->current.kind = TOK_IDENT;
      return true;
    }
    if (isdigit((unsigned char)c)) {
      size_t start = lx->pos++;
      while (isdigit((unsigned char)lx->src[lx->pos])) lx->pos++;
      lx->current.text = sDupRange(lx->src + start, lx->pos - start);
      if (!lx->current.text) return false;
      lx->current.kind = TOK_NUMBER;
      return true;
    }
    return false;
  }
}

static bool sExpect(Lexer *lx, TokenKind kind)
{
  if (lx->current.kind != kind) return false;
  return sNextToken(lx);
}

static bool sExpectString(Lexer *lx, char **out)
{
  if (lx->current.kind != TOK_STRING) return false;
  *out = UfCommandStrDup(lx->current.text);
  if (!*out) return false;
  return sNextToken(lx);
}

static bool sModifierName(const char *s, UfCommandModifier *out)
{
  if (strcmp(s, "CTRL") == 0) *out = UFCOMMAND_MOD_CTRL;
  else if (strcmp(s, "SHIFT") == 0) *out = UFCOMMAND_MOD_SHIFT;
  else if (strcmp(s, "ALT") == 0) *out = UFCOMMAND_MOD_ALT;
  else if (strcmp(s, "META") == 0) *out = UFCOMMAND_MOD_META;
  else if (strcmp(s, "NONE") == 0) *out = UFCOMMAND_MOD_NONE;
  else return false;
  return true;
}

static bool sParseModifiers(Lexer *lx, UfCommandModifier *out)
{
  UfCommandModifier m = UFCOMMAND_MOD_NONE;
  if (!sExpect(lx, TOK_LBRACE)) return false;
  if (lx->current.kind != TOK_RBRACE) {
    while (1) {
      if (lx->current.kind != TOK_STRING && lx->current.kind != TOK_IDENT) return false;
      UfCommandModifier one;
      if (!sModifierName(lx->current.text, &one)) return false;
      m = (UfCommandModifier)(m | one);
      if (!sNextToken(lx)) return false;
      if (lx->current.kind == TOK_RBRACE) break;
      if (!sExpect(lx, TOK_COMMA)) return false;
    }
  }
  if (!sExpect(lx, TOK_RBRACE)) return false;
  *out = m;
  return true;
}

static void sLoadedFree(LoadedConfig *c)
{
  for (size_t i = 0; i < c->alias_count; ++i) {
    free(c->aliases[i].alias);
    free(c->aliases[i].expansion);
  }
  for (size_t i = 0; i < c->binding_count; ++i) {
    free(c->bindings[i].key);
    free(c->bindings[i].command);
  }
  free(c->aliases);
  free(c->bindings);
  memset(c, 0, sizeof(*c));
}

static bool sAddAlias(LoadedConfig *c, char *alias, char *expansion)
{
  AliasConfig *p = realloc(c->aliases, (c->alias_count + 1) * sizeof(*p));
  if (!p) return false;
  c->aliases                   = p;
  c->aliases[c->alias_count++] = (AliasConfig){alias, expansion};
  return true;
}

static bool sAddBinding(LoadedConfig *c, UfCommandModifier m, char *key, char *command)
{
  BindingConfig *p = realloc(c->bindings, (c->binding_count + 1) * sizeof(*p));
  if (!p) return false;
  c->bindings                     = p;
  c->bindings[c->binding_count++] = (BindingConfig){m, key, command};
  return true;
}

static bool sSkipValue(Lexer *lx);

static bool sSkipTable(Lexer *lx)
{
  if (!sExpect(lx, TOK_LBRACE)) return false;
  int depth = 1;
  while (depth > 0) {
    if (lx->current.kind == TOK_EOF) return false;
    if (lx->current.kind == TOK_LBRACE) depth++;
    else if (lx->current.kind == TOK_RBRACE) {
      depth--;
      if (depth == 0) return sNextToken(lx);
    }
    if (depth > 0 && !sNextToken(lx)) return false;
  }
  return true;
}

static bool sSkipValue(Lexer *lx)
{
  if (lx->current.kind == TOK_LBRACE) return sSkipTable(lx);
  if (lx->current.kind == TOK_STRING || lx->current.kind == TOK_IDENT || lx->current.kind == TOK_NUMBER) return
    sNextToken(lx);
  return false;
}

static bool sParseAliases(Lexer *lx, LoadedConfig *cfg)
{
  if (!sExpect(lx, TOK_LBRACE)) return false;
  while (lx->current.kind != TOK_RBRACE) {
    char *alias = NULL, *expansion = NULL;
    if (!sExpect(lx, TOK_LBRACKET)) return false;
    if (!sExpectString(lx, &alias)) {
      free(alias);
      return false;
    }
    if (!sExpect(lx, TOK_RBRACKET) || !sExpect(lx, TOK_EQUAL) || !sExpectString(lx, &expansion)) {
      free(alias);
      free(expansion);
      return false;
    }
    if (!sAddAlias(cfg, alias, expansion)) {
      free(alias);
      free(expansion);
      return false;
    }
    if (lx->current.kind == TOK_COMMA) { if (!sNextToken(lx)) return false; }
    else if (lx->current.kind != TOK_RBRACE) return false;
  }
  return sExpect(lx, TOK_RBRACE);
}

static bool sParseBinding(Lexer *lx, LoadedConfig *cfg)
{
  if (!sExpect(lx, TOK_LBRACE)) return false;
  UfCommandModifier m   = UFCOMMAND_MOD_NONE;
  char *            key = NULL, *command = NULL;
  while (lx->current.kind != TOK_RBRACE) {
    if (lx->current.kind != TOK_IDENT) {
      free(key);
      free(command);
      return false;
    }
    char *field = UfCommandStrDup(lx->current.text);
    if (!field) {
      free(key);
      free(command);
      return false;
    }
    if (!sNextToken(lx) || !sExpect(lx, TOK_EQUAL)) {
      free(field);
      free(key);
      free(command);
      return false;
    }
    bool ok = true;
    if (strcmp(field, "modifiers") == 0) ok = sParseModifiers(lx, &m);
    else if (strcmp(field, "key") == 0) {
      free(key);
      key = NULL;
      ok  = sExpectString(lx, &key);
    }
    else if (strcmp(field, "command") == 0) {
      free(command);
      command = NULL;
      ok      = sExpectString(lx, &command);
    }
    else ok = sSkipValue(lx);
    free(field);
    if (!ok) {
      free(key);
      free(command);
      return false;
    }
    if (lx->current.kind == TOK_COMMA) {
      if (!sNextToken(lx)) {
        free(key);
        free(command);
        return false;
      }
    }
    else if (lx->current.kind != TOK_RBRACE) {
      free(key);
      free(command);
      return false;
    }
  }
  if (!sExpect(lx, TOK_RBRACE) || !key || !command) {
    free(key);
    free(command);
    return false;
  }
  if (!sAddBinding(cfg, m, key, command)) {
    free(key);
    free(command);
    return false;
  }
  return true;
}

static bool sParseBindings(Lexer *lx, LoadedConfig *cfg)
{
  if (!sExpect(lx, TOK_LBRACE)) return false;
  while (lx->current.kind != TOK_RBRACE) {
    if (!sParseBinding(lx, cfg)) return false;
    if (lx->current.kind == TOK_COMMA) { if (!sNextToken(lx)) return false; }
    else if (lx->current.kind != TOK_RBRACE) return false;
  }
  return sExpect(lx, TOK_RBRACE);
}

static bool sParseKeyConfig(Lexer *lx, LoadedConfig *cfg)
{
  if (!sExpect(lx, TOK_LBRACE)) return false;
  while (lx->current.kind != TOK_RBRACE) {
    if (lx->current.kind != TOK_IDENT) return false;
    char *field = UfCommandStrDup(lx->current.text);
    if (!field) return false;
    if (!sNextToken(lx) || !sExpect(lx, TOK_EQUAL)) {
      free(field);
      return false;
    }
    bool ok = true;
    if (strcmp(field, "default_modifiers") == 0) {
      ok = sParseModifiers(lx, &cfg->key_config.default_modifiers);
      if (ok) cfg->key_config_seen = true;
    }
    else ok = sSkipValue(lx);
    free(field);
    if (!ok) return false;
    if (lx->current.kind == TOK_COMMA) { if (!sNextToken(lx)) return false; }
    else if (lx->current.kind != TOK_RBRACE) return false;
  }
  return sExpect(lx, TOK_RBRACE);
}

static bool sParseDocument(const char *src, LoadedConfig *cfg)
{
  Lexer lx = {.src = src, .line = 1};
  if (!sNextToken(&lx)) return false;
  if (lx.current.kind != TOK_IDENT || strcmp(lx.current.text, "return") != 0) {
    sTokenFree(&lx.current);
    return false;
  }
  if (!sNextToken(&lx) || lx.current.kind != TOK_LBRACE) {
    sTokenFree(&lx.current);
    return false;
  }
  if (!sNextToken(&lx)) {
    sTokenFree(&lx.current);
    return false;
  }
  while (lx.current.kind != TOK_RBRACE) {
    if (lx.current.kind != TOK_IDENT) {
      sTokenFree(&lx.current);
      return false;
    }
    char *field = UfCommandStrDup(lx.current.text);
    if (!field || !sNextToken(&lx) || !sExpect(&lx, TOK_EQUAL)) {
      free(field);
      sTokenFree(&lx.current);
      return false;
    }
    bool ok;
    if (strcmp(field, "aliases") == 0) ok = sParseAliases(&lx, cfg);
    else if (strcmp(field, "bindings") == 0) ok = sParseBindings(&lx, cfg);
    else if (strcmp(field, "key_config") == 0) ok = sParseKeyConfig(&lx, cfg);
    else if (strcmp(field, "version") == 0) {
      if (lx.current.kind != TOK_NUMBER) ok = false;
      else {
        char *        end = NULL;
        unsigned long v   = strtoul(lx.current.text, &end, 10);
        ok                = end != lx.current.text && *end == '\0' && v <= UINT_MAX;
        if (ok) {
          cfg->version      = (unsigned)v;
          cfg->version_seen = true;
          ok                = sNextToken(&lx);
        }
      }
    }
    else ok = sSkipValue(&lx);
    free(field);
    if (!ok) {
      sTokenFree(&lx.current);
      return false;
    }
    if (lx.current.kind == TOK_COMMA) {
      if (!sNextToken(&lx)) {
        sTokenFree(&lx.current);
        return false;
      }
    }
    else if (lx.current.kind != TOK_RBRACE) {
      sTokenFree(&lx.current);
      return false;
    }
  }
  if (!sNextToken(&lx) || lx.current.kind != TOK_EOF) {
    sTokenFree(&lx.current);
    return false;
  }
  sTokenFree(&lx.current);
  return true;
}

static bool sFileReadAll(const char *path, char **out)
{
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return false;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return false;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return false;
  }
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return false;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  int    err = ferror(f);
  fclose(f);
  if (err || got != (size_t)n) {
    free(buf);
    return false;
  }
  buf[n] = '\0';
  *out   = buf;
  return true;
}

static int sCmpAlias(const void *a, const void *b)
{
  const UfCommandAliasPrivate *const *x = a, *const *y = b;
  return strcmp((*x)->name, (*y)->name);
}

static int sCmpBinding(const void *a, const void *b)
{
  const UfCommandBindingPrivate *const *x = a, *const *y = b;
  if ((*x)->modifiers != (*y)->modifiers) return (*x)->modifiers < (*y)->modifiers ? -1 : 1;
  return strcmp((*x)->key, (*y)->key);
}

static bool sWriteEscaped(FILE *f, const char *s)
{
  if (fputc('"', f) == EOF) return false;
  for (; *s; ++s) {
    unsigned char c   = (unsigned char)*s;
    const char *  esc = NULL;
    switch (c) {
    case '\n': esc = "\\n";
      break;
    case '\r': esc = "\\r";
      break;
    case '\t': esc = "\\t";
      break;
    case '\\': esc = "\\\\";
      break;
    case '"': esc = "\\\"";
      break;
    default: break;
    }
    if (esc) { if (fputs(esc, f) == EOF) return false; }
    else if (c < 0x20u) { if (fprintf(f, "\\x%02X", (unsigned)c) < 0) return false; }
    else if (fputc(c, f) == EOF) return false;
  }
  return fputc('"', f) != EOF;
}

static bool sWriteModifiers(FILE *f, UfCommandModifier m)
{
  const struct
  {
    UfCommandModifier bit;
    const char *      name;
  } names[] = {
    {UFCOMMAND_MOD_CTRL, "CTRL"}, {UFCOMMAND_MOD_SHIFT, "SHIFT"}, {UFCOMMAND_MOD_ALT, "ALT"},
    {UFCOMMAND_MOD_META, "META"}
  };
  if (fputs("{ ", f) == EOF) return false;
  bool first = true;
  if (m == UFCOMMAND_MOD_NONE) { if (fputs("\"NONE\"", f) == EOF) return false; }
  else
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
      if ((m & names[i].bit) != 0u) {
        if (!first && fputs(", ", f) == EOF) return false;
        if (fputc('"', f) == EOF || fputs(names[i].name, f) == EOF || fputc('"', f) == EOF) return false;
        first = false;
      }
  return fputs(" }", f) != EOF;
}

static bool sWriteConfig(FILE *f, const UfCommandRegistry *r)
{
  const UfCommandAliasPrivate **  aliases  = NULL;
  const UfCommandBindingPrivate **bindings = NULL;
  if (r->alias_count) {
    aliases = malloc(r->alias_count * sizeof(*aliases));
    if (!aliases) return false;
    size_t n = 0;
    for (size_t i = 0; i < r->alias_capacity; ++i) if (r->aliases[i].occupied) aliases[n++] = &r->aliases[i];
    qsort(aliases, n, sizeof(*aliases), sCmpAlias);
  }
  if (r->binding_count) {
    bindings = malloc(r->binding_count * sizeof(*bindings));
    if (!bindings) {
      free(aliases);
      return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < r->binding_capacity; ++i) if (r->bindings[i].occupied) bindings[n++] = &r->bindings[i];
    qsort(bindings, n, sizeof(*bindings), sCmpBinding);
  }
  bool ok = fputs("-- UfCommand user configuration (Lua-compatible)\nreturn {\n  version = 1,\n", f) != EOF;
  ok      = ok && fputs("  key_config = { default_modifiers = ", f) != EOF;
  ok      = ok && sWriteModifiers(f, r->key_config.default_modifiers);
  ok      = ok && fputs(" },\n", f) != EOF;
  ok      = ok && fputs("  aliases = {\n", f) != EOF;
  for (size_t i = 0; ok && i < r->alias_count; ++i) {
    ok = fputs("    [", f) != EOF && sWriteEscaped(f, aliases[i]->name) && fputs("] = ", f) != EOF &&
      sWriteEscaped(f, aliases[i]->expansion) && fputs(",\n", f) != EOF;
  }
  ok = ok && fputs("  },\n  bindings = {\n", f) != EOF;
  for (size_t i = 0; ok && i < r->binding_count; ++i) {
    ok = fputs("    { modifiers = ", f) != EOF && sWriteModifiers(f, bindings[i]->modifiers) && fputs(", key = ", f) !=
      EOF && sWriteEscaped(f, bindings[i]->key) && fputs(", command = ", f) != EOF &&
      sWriteEscaped(f, bindings[i]->command) && fputs(" },\n", f) != EOF;
  }
  ok = ok && fputs("  },\n}\n", f) != EOF;
  free(aliases);
  free(bindings);
  return ok;
}

UfCommandStatus UfCommandRegistrySaveUserConfig(const UfCommandRegistry *registry, const char *path)
{
  if (!registry || !path || !*path) return UFCOMMAND_INVALID_ARGUMENT;
  size_t n = strlen(path);
  if (n > SIZE_MAX - 5) return UFCOMMAND_NO_MEMORY;
  char *tmp = malloc(n + 5);
  if (!tmp) return UFCOMMAND_NO_MEMORY;
  (void)snprintf(tmp, n + 5, "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) {
    free(tmp);
    return UFCOMMAND_IO_ERROR;
  }
  bool ok = sWriteConfig(f, registry);
  if (ok && fflush(f) != 0) ok = false;
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    UFCOMMAND_LOG_ERROR(registry, "save: could not write '%s'", tmp);
    remove(tmp);
    free(tmp);
    return UFCOMMAND_IO_ERROR;
  }
  if (rename(tmp, path) != 0) {
    UFCOMMAND_LOG_ERROR(registry, "save: could not replace '%s'", path);
    remove(tmp);
    free(tmp);
    return UFCOMMAND_IO_ERROR;
  }
  free(tmp);
  UFCOMMAND_LOG_INFO(registry, "user configuration saved to '%s'", path);
  return UFCOMMAND_OK;
}

static UfCommandStatus sCloneRegistry(const UfCommandRegistry *src, UfCommandRegistry **out)
{
  UfCommandRegistryConfig cfg = {0};
  cfg.initial_capacity        = src->command_count + src->alias_count + src->binding_count + 16;
  cfg.max_command_components  = src->max_command_components;
  UfCommandRegistry *clone    = NULL;
  UfCommandStatus    st       = UfCommandRegistryCreate(&cfg, &clone);
  if (st != UFCOMMAND_OK) return st;
  for (size_t i = 0; i < src->command_capacity && st == UFCOMMAND_OK; ++i)
    if (src->commands[i].occupied) {
      UfCommandDefinition d = {
        src->commands[i].name, src->commands[i].description, src->commands[i].handler, src->commands[i].user_data,
        src->commands[i].min_args, src->commands[i].max_args
      };
      st = UfCommandRegistryAdd(clone, &d);
    }
  for (size_t i = 0; i < src->alias_capacity && st == UFCOMMAND_OK; ++i)
    if (src->aliases[i].occupied) {
      UfCommandAliasDefinition d = {src->aliases[i].name, src->aliases[i].expansion};
      st                         = UfCommandRegistryAddAlias(clone, &d);
    }
  for (size_t i = 0; i < src->binding_capacity && st == UFCOMMAND_OK; ++i)
    if (src->bindings[i].occupied) {
      UfCommandBinding b = {src->bindings[i].modifiers, src->bindings[i].key, src->bindings[i].command};
      st                 = UfCommandRegistryAddBinding(clone, &b);
    }
  if (st == UFCOMMAND_OK) clone->key_config = src->key_config;
  // Non-entry state must be carried across explicitly: this rebuilds the
  // registry from its entries, so anything set on the original and not listed
  // here is lost when LoadUserConfig swaps the clone in.
  if (st == UFCOMMAND_OK) clone->uf_logger = src->uf_logger;
  if (st != UFCOMMAND_OK) {
    UfCommandRegistryDestroy(clone);
    return st;
  }
  *out = clone;
  return UFCOMMAND_OK;
}

UfCommandStatus UfCommandRegistryLoadUserConfig(UfCommandRegistry *registry, const char *path)
{
  if (!registry || !path || !*path) return UFCOMMAND_INVALID_ARGUMENT;
  // The staging clone frees and replaces every table, so this is the most
  // destructive mutation there is: it may not run under a dispatch or a
  // suggestion, both of which iterate those tables.
  if (registry->dispatch_depth != 0) return UFCOMMAND_CONFLICT;
  char *src = NULL;
  if (!sFileReadAll(path, &src)) {
    UFCOMMAND_LOG_ERROR(registry, "load: cannot read '%s'", path);
    return UFCOMMAND_IO_ERROR;
  }
  LoadedConfig cfg    = {0};
  bool         parsed = sParseDocument(src, &cfg);
  free(src);
  if (!parsed) {
    UFCOMMAND_LOG_ERROR(registry, "load: '%s' is not a valid configuration document", path);
    sLoadedFree(&cfg);
    return UFCOMMAND_CONFIG_ERROR;
  }
  if (!cfg.version_seen) {
    UFCOMMAND_LOG_ERROR(registry, "load: '%s' declares no version", path);
    sLoadedFree(&cfg);
    return UFCOMMAND_CONFIG_ERROR;
  }
  if (cfg.version != 1u) {
    UFCOMMAND_LOG_ERROR(registry, "load: '%s' declares unsupported version %u", path, cfg.version);
    sLoadedFree(&cfg);
    return UFCOMMAND_UNSUPPORTED_VERSION;
  }
  for (size_t i = 0; i < cfg.alias_count; ++i) {
    if (!cfg.aliases[i].alias[0] || !cfg.aliases[i].expansion[0]) {
      sLoadedFree(&cfg);
      return UFCOMMAND_CONFIG_ERROR;
    }
    for (size_t j = i + 1; j < cfg.alias_count; ++j) if (strcmp(cfg.aliases[i].alias, cfg.aliases[j].alias) == 0) {
      sLoadedFree(&cfg);
      return UFCOMMAND_DUPLICATE;
    }
  }
  for (size_t i = 0; i < cfg.binding_count; ++i) {
    if (!cfg.bindings[i].key[0] || !cfg.bindings[i].command[0]) {
      sLoadedFree(&cfg);
      return UFCOMMAND_CONFIG_ERROR;
    }
    for (size_t j = i + 1; j < cfg.binding_count; ++j) if (cfg.bindings[i].modifiers == cfg.bindings[j].modifiers &&
      strcmp(cfg.bindings[i].key, cfg.bindings[j].key) == 0) {
      sLoadedFree(&cfg);
      return UFCOMMAND_DUPLICATE;
    }
  }
  UfCommandRegistry *stage = NULL;
  UfCommandStatus    st    = sCloneRegistry(registry, &stage);
  if (st != UFCOMMAND_OK) {
    sLoadedFree(&cfg);
    return st;
  }
  // sCloneRegistry hands the clone a zeroed config, so it must arrive with no
  // engine of its own -- and never with a copy of this registry's, which the
  // transfer below would otherwise orphan.
  if (stage->completion && stage->completion != registry->completion)
    UfCommandCompletionDestroy(stage->completion);
  for (size_t i = 0; i < cfg.alias_count && st == UFCOMMAND_OK; ++i) {
    UfCommandAliasDefinition d = {cfg.aliases[i].alias, cfg.aliases[i].expansion};
    st                         = UfCommandRegistryAddAlias(stage, &d);
  }
  for (size_t i = 0; i < cfg.binding_count && st == UFCOMMAND_OK; ++i) {
    UfCommandBinding b = {cfg.bindings[i].modifiers, cfg.bindings[i].key, cfg.bindings[i].command};
    st                 = UfCommandRegistryAddBinding(stage, &b);
  }
  if (st == UFCOMMAND_OK && cfg.key_config_seen) st = UfCommandRegistrySetKeyConfig(stage, &cfg.key_config);
  if (st != UFCOMMAND_OK) {
    UfCommandRegistryDestroy(stage);
    sLoadedFree(&cfg);
    return st;
  }

  UfCommandRegistry *old = malloc(sizeof(*old));
  if (!old) {
    UfCommandRegistryDestroy(stage);
    sLoadedFree(&cfg);
    return UFCOMMAND_NO_MEMORY;
  }
  // No failure may follow this point: the transfer below leaves the host's handle live.
  *old      = *registry;
  *registry = *stage;
  free(stage);
  // Carry what the clone did not rebuild, so Destroy(old) cannot free it.
  registry->telemetry       = old->telemetry;
  registry->telemetry_owned = old->telemetry_owned;
  old->telemetry            = NULL;
  old->telemetry_owned      = false;
  registry->completion      = old->completion;
  old->completion           = NULL;
  UfCommandRegistryDestroy(old);
  sLoadedFree(&cfg);
  UFCOMMAND_LOG_INFO(registry, "user configuration loaded from '%s'", path);
  return UFCOMMAND_OK;
}
