/**
 * @file ufconfig_parse.c
 * @brief The parser for the Lua-styled document format, and the node tree it produces.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ufconfig_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  const char *s;
  size_t      n,    i;
  int         line, col;
  UfConfig *  h;
  int         depth;
} P;

static char sPeek(P *p) { return p->i < p->n ? p->s[p->i] : 0; }
static char sPeekn(P *p, size_t k) { return p->i + k < p->n ? p->s[p->i + k] : 0; }

static char sGetc(P *p)
{
  if (p->i >= p->n) return 0;
  char c = p->s[p->i++];
  if (c == '\n') {
    p->line++;
    p->col = 1;
  }
  else p->col++;
  return c;
}

static void sSkipWsComments(P *p)
{
  for (;;) {
    char c = sPeek(p);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      sGetc(p);
      continue;
    }
    if (c == '-' && sPeekn(p, 1) == '-') {
      sGetc(p);
      sGetc(p);
      if (sPeek(p) == '[' && sPeekn(p, 1) == '[') {
        sGetc(p);
        sGetc(p);
        while (sPeek(p) && !(sPeek(p) == ']' && sPeekn(p, 1) == ']')) sGetc(p);
        if (sPeek(p) == ']') {
          sGetc(p);
          sGetc(p);
        }
      }
      else {
        while (sPeek(p) && sPeek(p) != '\n') sGetc(p);
      }
      continue;
    }
    break;
  }
}

static int sIsIdentStart(char c)
{
  return ConfigAsciiIsAlpha((unsigned char)c) || c == '_';
}

static int sIsIdent(char c)
{
  return sIsIdentStart(c) || ConfigAsciiIsDigit((unsigned char)c);
}

static UfNode *sMk(P *p, UfNodeKind k, const char *name, size_t nlen)
{
  UfNode *n = (UfNode*)ConfigArenaAlloc(&p->h->arena, sizeof(UfNode));
  if (!n) return NULL;
  memset(n, 0, sizeof(*n));
  n->kind   = k;
  n->line   = p->line;
  n->column = p->col;
  if (name) n->name = ConfigArenaStrndup(&p->h->arena, name, nlen);
  return n;
}

static void sAddChild(UfNode *parent, UfNode *c)
{
  c->parent = parent;
  if (!parent->first_child) parent->first_child = c;
  else {
    UfNode *x = parent->first_child;
    while (x->next_sibling) x = x->next_sibling;
    x->next_sibling = c;
  }
}

static UfConfigStatus sParseValue(P *p, UfNode **out);

static UfConfigStatus sRejectExpr(P *p, const char *name)
{
  ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, name, p->line, p->col, "evaluation construct rejected: %s", name);
  return UF_CONFIG_ERR_PARSE;
}

static UfConfigStatus sParseString(P *p, char **out, size_t *olen)
{
  char   q   = sGetc(p);
  size_t cap = 64, n = 0;
  char * buf = (char*)malloc(cap);
  if (!buf) return UF_CONFIG_ERR_NO_MEMORY;
  while (sPeek(p) && sPeek(p) != q) {
    char c = sGetc(p);
    if (c == '\\') {
      char e = sGetc(p);
      switch (e) {
      case 'n': c = '\n';
        break;
      case 'r': c = '\r';
        break;
      case 't': c = '\t';
        break;
      case '\\': c = '\\';
        break;
      case '"': c = '"';
        break;
      case '\'': c = '\'';
        break;
      default: c = e;
        break;
      }
    }
    if (n + 1 >= cap) {
      cap      *= 2;
      char *nb = (char*)realloc(buf, cap);
      if (!nb) {
        free(buf);
        return UF_CONFIG_ERR_NO_MEMORY;
      }
      buf = nb;
    }
    if (n >= PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_STRING_BYTES) {
      free(buf);
      ConfigSetError(UF_CONFIG_ERR_LIMIT_EXCEEDED, NULL, NULL, p->line, p->col, "string too long");
      return UF_CONFIG_ERR_LIMIT_EXCEEDED;
    }
    buf[n++] = c;
  }
  if (sPeek(p) != q) {
    free(buf);
    ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, NULL, p->line, p->col, "unterminated string");
    return UF_CONFIG_ERR_PARSE;
  }
  sGetc(p);
  buf[n] = 0;
  *out   = ConfigArenaStrndup(&p->h->arena, buf, n);
  *olen  = n;
  free(buf);
  return *out ? UF_CONFIG_OK : UF_CONFIG_ERR_NO_MEMORY;
}

static UfConfigStatus sParseTable(P *p, UfNode *tbl);

static UfConfigStatus sParseValue(P *p, UfNode **out)
{
  sSkipWsComments(p);
  if (p->depth > PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_DEPTH) {
    ConfigSetError(UF_CONFIG_ERR_LIMIT_EXCEEDED, NULL, NULL, p->line, p->col, "max depth");
    return UF_CONFIG_ERR_LIMIT_EXCEEDED;
  }
  char c = sPeek(p);
  if (c == '#') return sRejectExpr(p, "length-operator");
  if (c == '"' || c == '\'') {
    char *         s;
    size_t         n;
    UfConfigStatus st = sParseString(p, &s, &n);
    if (st) return st;
    sSkipWsComments(p);
    if (sPeek(p) == '.' && sPeekn(p, 1) == '.') return sRejectExpr(p, "concat");
    UfNode *nd    = sMk(p, UF_NODE_LEAF, NULL, 0);
    nd->vkind     = UF_CONFIG_KIND_STRING;
    nd->v.s.raw     = s;
    nd->v.s.raw_len = n;
    nd->v.s.eff     = s;
    nd->v.s.eff_len = n;
    *out          = nd;
    return UF_CONFIG_OK;
  }
  if (c == '{') {
    p->depth++;
    UfNode *       nd = sMk(p, UF_NODE_SCOPE, NULL, 0);
    UfConfigStatus st = sParseTable(p, nd);
    p->depth--;
    *out = nd;
    return st;
  }
  if (sIsIdentStart(c)) {
    size_t start = p->i;
    while (sIsIdent(sPeek(p))) sGetc(p);
    size_t      len = p->i - start;
    const char *id  = p->s + start;
    if (len == 4 && memcmp(id, "true", 4) == 0) {
      UfNode *nd = sMk(p, UF_NODE_LEAF, NULL, 0);
      nd->vkind  = UF_CONFIG_KIND_BOOL;
      nd->v.b.raw  = nd->v.b.eff = true;
      *out       = nd;
      return UF_CONFIG_OK;
    }
    if (len == 5 && memcmp(id, "false", 5) == 0) {
      UfNode *nd = sMk(p, UF_NODE_LEAF, NULL, 0);
      nd->vkind  = UF_CONFIG_KIND_BOOL;
      nd->v.b.raw  = nd->v.b.eff = false;
      *out       = nd;
      return UF_CONFIG_OK;
    }
    if (len == 3 && memcmp(id, "nil", 3) == 0) {
      UfNode *nd = sMk(p, UF_NODE_LEAF, NULL, 0);
      nd->vkind  = UF_CONFIG_KIND_ABSENT;
      *out       = nd;
      return UF_CONFIG_OK;
    }
    if ((len == 3 && memcmp(id, "and", 3) == 0) || (len == 3 && memcmp(id, "not", 3) == 0) || (len == 2 &&
      memcmp(id, "or", 2) == 0)) return sRejectExpr(p, "boolean-operator");
    sSkipWsComments(p);
    if (sPeek(p) == '(') return sRejectExpr(p, "call");
    UfNode *nd       = sMk(p, UF_NODE_ALIAS, NULL, 0);
    nd->alias_target = ConfigArenaStrndup(&p->h->arena, id, len);
    *out             = nd;
    return UF_CONFIG_OK;
  }
  if (c == '-' || ConfigAsciiIsDigit((unsigned char)c)) {
    int neg = 0;
    if (c == '-') {
      neg = 1;
      sGetc(p);
    }
    if (!ConfigAsciiIsDigit((unsigned char)sPeek(p))) {
      ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, NULL, p->line, p->col, "expected number");
      return UF_CONFIG_ERR_PARSE;
    }
    int64_t v = 0;
    while (ConfigAsciiIsDigit((unsigned char)sPeek(p))) {
      v = v * 10 + (sPeek(p) - '0');
      sGetc(p);
    }
    if (sPeek(p) == '.' || sPeek(p) == 'e' || sPeek(p) == 'E') {
      double f = (double)v;
      if (sPeek(p) == '.') {
        sGetc(p);
        double place = 0.1;
        while (ConfigAsciiIsDigit((unsigned char)sPeek(p))) {
          f     += (sPeek(p) - '0') * place;
          place *= 0.1;
          sGetc(p);
        }
      }
      if (neg) f = -f;
      UfNode *nd = sMk(p, UF_NODE_LEAF, NULL, 0);
      nd->vkind  = UF_CONFIG_KIND_FLOAT;
      nd->v.f.raw  = nd->v.f.eff = f;
      *out       = nd;
      return UF_CONFIG_OK;
    }
    if (neg) v = -v;
    sSkipWsComments(p);
    char nx = sPeek(p);
    if (nx == '+' || nx == '*' || nx == '/' || nx == '%') return sRejectExpr(p, "arithmetic");
    if (nx == '<' || nx == '>' || nx == '=') return sRejectExpr(p, "comparison");
    UfNode *nd = sMk(p, UF_NODE_LEAF, NULL, 0);
    nd->vkind  = UF_CONFIG_KIND_INT;
    nd->v.i.raw  = nd->v.i.eff = v;
    *out       = nd;
    return UF_CONFIG_OK;
  }
  if (c == '[') return sRejectExpr(p, "bracket-form");
  ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, NULL, p->line, p->col, "unexpected byte");
  return UF_CONFIG_ERR_PARSE;
}

static UfConfigStatus sParseTable(P *p, UfNode *tbl)
{
  sSkipWsComments(p);
  if (sPeek(p) != '{') {
    ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, NULL, p->line, p->col, "expected '{'");
    return UF_CONFIG_ERR_PARSE;
  }
  sGetc(p);
  size_t anon = 0;
  /* The tail of the child list, so each element links on in constant time
     rather than walking to the end.  tbl is freshly made and therefore empty,
     so this starts NULL and document order is preserved exactly as a tail
     append would have produced it.  Walking instead made parsing a table
     quadratic in its width, which the document's author chooses. */
  UfNode *tail = NULL;
  for (;;) {
    sSkipWsComments(p);
    if (sPeek(p) == '}') {
      sGetc(p);
      goto done_table;
    }
    if (!sPeek(p)) {
      ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, NULL, p->line, p->col, "unterminated table");
      return UF_CONFIG_ERR_PARSE;
    }
    char   namebuf[256];
    size_t nlen = 0;
    if (sIsIdentStart(sPeek(p))) {
      while (sIsIdent(sPeek(p)) && nlen + 1 < sizeof(namebuf)) namebuf[nlen++] = sGetc(p);
      namebuf[nlen] = 0;
      sSkipWsComments(p);
      if (sPeek(p) != '=') {
        ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, NULL, p->line, p->col, "expected '=' after key");
        return UF_CONFIG_ERR_PARSE;
      }
      sGetc(p);
    }
    else {
      snprintf(namebuf, sizeof(namebuf), "%zu", anon++);
      nlen = strlen(namebuf);
    }
    UfNode *       val = NULL;
    UfConfigStatus st  = sParseValue(p, &val);
    if (st) return st;
    val->name = ConfigArenaStrndup(&p->h->arena, namebuf, nlen);
    val->parent = tbl;
    if (tail) tail->next_sibling = val;
    else tbl->first_child = val;
    tail = val;
    sSkipWsComments(p);
    if (sPeek(p) == ',') {
      sGetc(p);
      continue;
    }
    if (sPeek(p) == '}') {
      sGetc(p);
      goto done_table;
    }
    ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, NULL, p->line, p->col, "expected ',' or '}'");
    return UF_CONFIG_ERR_PARSE;
  }
done_table: {
    size_t idx = 0, all_num = 1, any = 0;
    for (UfNode *c = tbl->first_child; c; c = c->next_sibling) {
      any = 1;
      char expect[32];
      snprintf(expect, sizeof(expect), "%zu", idx);
      if (!c->name || strcmp(c->name, expect) != 0) {
        all_num = 0;
        break;
      }
      idx++;
    }
    if (any && all_num) {
      tbl->kind        = UF_NODE_ARRAY;
      tbl->array_count = idx;
    }
  }
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigParseLuaStyle(UfConfig *h, const char *src, size_t len, UfNode **out_root)
{
  P       p        = {.s = src, .n = len, .i = 0, .line = 1, .col = 1, .h = h, .depth = 0};
  UfNode *bindings = sMk(&p, UF_NODE_SCOPE, "", 0);
  UfNode *ret_root = NULL;
  sSkipWsComments(&p);
  UfNode *bindings_tail = NULL;
  while (p.i < p.n) {
    sSkipWsComments(&p);
    if (p.i >= p.n) break;
    if (sIsIdentStart(sPeek(&p))) {
      size_t start = p.i;
      while (sIsIdent(sPeek(&p))) sGetc(&p);
      size_t      nlen = p.i - start;
      const char *id   = src + start;
      sSkipWsComments(&p);
      if (nlen == 6 && memcmp(id, "return", 6) == 0) {
        UfNode *       v  = NULL;
        UfConfigStatus st = sParseValue(&p, &v);
        if (st) return st;
        ret_root = v;
        continue;
      }
      if (sPeek(&p) != '=') {
        ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, NULL, p.line, p.col, "expected '=' in binding");
        return UF_CONFIG_ERR_PARSE;
      }
      sGetc(&p);
      sSkipWsComments(&p);
      UfNode *       v  = NULL;
      UfConfigStatus st = sParseValue(&p, &v);
      if (st) return st;
      sSkipWsComments(&p);
      char nx = sPeek(&p);
      if (nx == '+' || nx == '*' || nx == '/' || (nx == '.' && sPeekn(&p, 1) == '.')) return sRejectExpr(
        &p, nx == '.' ? "concat" : "arithmetic");
      v->name = ConfigArenaStrndup(&h->arena, id, nlen);
      v->parent = bindings;
      if (bindings_tail) bindings_tail->next_sibling = v;
      else bindings->first_child = v;
      bindings_tail = v;
      continue;
    }
    ConfigSetError(UF_CONFIG_ERR_PARSE, NULL, NULL, p.line, p.col, "expected binding or return");
    return UF_CONFIG_ERR_PARSE;
  }
  if (ret_root) {
    ret_root->parent  = bindings;
    *out_root         = ret_root;
    UfNode *hold      = sMk(&p, UF_NODE_SCOPE, "#bindings", 10);
    hold->first_child = bindings->first_child;
    sAddChild(ret_root, hold);
  }
  else {
    *out_root = bindings;
  }
  return UF_CONFIG_OK;
}

static UfNode *sFindBinding(UfNode *root, const char *name)
{
  if (!root || !name) return NULL;
  for (UfNode *c = root->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) {
      for (UfNode *b = c->first_child; b; b = b->next_sibling)
        if (b->name && strcmp(b->name, name) == 0 && b->kind != UF_NODE_ALIAS) return b;
      for (UfNode *b = c->first_child; b; b = b->next_sibling)
        if (b->name && strcmp(b->name, name) == 0) return b;
    }
  }
  UfNode *top = root;
  while (top->parent) top = top->parent;
  if (top != root) return sFindBinding(top, name);
  return NULL;
}

static UfConfigStatus sResolveOne(UfConfig *h, UfNode *n, int depth, UfNode **stack, int sp);

static UfConfigStatus sResolveTree(UfConfig *h, UfNode *n, int depth, UfNode **stack, int sp)
{
  for (UfNode *c = n->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) {
      for (UfNode *b = c->first_child; b; b = b->next_sibling) {
        UfConfigStatus st = sResolveOne(h, b, depth, stack, sp);
        if (st) return st;
      }
      continue;
    }
    UfConfigStatus st = sResolveOne(h, c, depth, stack, sp);
    if (st) return st;
  }
  return UF_CONFIG_OK;
}

static UfConfigStatus sResolveOne(UfConfig *h, UfNode *n, int depth, UfNode **stack, int sp)
{
  if (n->kind == UF_NODE_ALIAS) {
    if (depth >= PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_ALIAS_DEPTH) {
      ConfigSetError(UF_CONFIG_ERR_ALIAS_DEPTH_EXCEEDED, n->name, NULL, n->line, n->column, "alias depth");
      return UF_CONFIG_ERR_ALIAS_DEPTH_EXCEEDED;
    }
    for (int i = 0; i < sp; i++) {
      if (stack[i] == n) {
        char   path[256] = {0};
        size_t off       = 0;
        for (int j = i; j < sp; j++)
          off += (size_t)snprintf(path + off, sizeof(path) - off, "%s -> ",
                                  stack[j]->name ?
                                    stack[j]->name :
                                    (stack[j]->alias_target ? stack[j]->alias_target : "?"));
        snprintf(path + off, sizeof(path) - off, "%s", n->name ? n->name : n->alias_target);
        ConfigSetError(UF_CONFIG_ERR_CYCLE, n->name, NULL, n->line, n->column, "cycle: %s", path);
        return UF_CONFIG_ERR_CYCLE;
      }
    }
    UfNode *tgt = sFindBinding(h->root ? h->root : n, n->alias_target);
    if (!tgt) {
      UfNode *top = n;
      while (top->parent) top = top->parent;
      tgt = sFindBinding(top, n->alias_target);
    }
    if (!tgt && h->root) tgt = sFindBinding(h->root, n->alias_target);
    if (!tgt) {
      ConfigSetError(UF_CONFIG_ERR_ALIAS_UNRESOLVED, n->name, NULL, n->line, n->column,
                     "alias '%s' unresolved (target '%s')", n->name ? n->name : "?", n->alias_target);
      return UF_CONFIG_ERR_ALIAS_UNRESOLVED;
    }
    if (tgt->kind != UF_NODE_SCOPE && tgt->kind != UF_NODE_ALIAS) {
      ConfigSetError(UF_CONFIG_ERR_NOT_A_TABLE, n->name, NULL, n->line, n->column, "alias target is not a table");
      return UF_CONFIG_ERR_NOT_A_TABLE;
    }
    stack[sp] = n;
    if (tgt->kind == UF_NODE_ALIAS) {
      UfConfigStatus st = sResolveOne(h, tgt, depth + 1, stack, sp + 1);
      if (st) return st;
      n->resolved = tgt->resolved ? tgt->resolved : tgt;
    }
    else {
      n->resolved       = tgt;
      UfConfigStatus st = sResolveTree(h, tgt, depth + 1, stack, sp + 1);
      if (st) return st;
    }
    return UF_CONFIG_OK;
  }
  if (n->kind == UF_NODE_SCOPE) return sResolveTree(h, n, depth, stack, sp);
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigResolveAliases(UfConfig *h, UfNode *root)
{
  UfNode *stack[PRIV_CONFIG_DEFAULT_UFCONFIG_MAX_ALIAS_DEPTH + 4];
  h->root = root;
  for (UfNode *c = root->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) {
      for (UfNode *b = c->first_child; b; b = b->next_sibling) {
        UfConfigStatus st = sResolveOne(h, b, 0, stack, 0);
        if (st) return st;
      }
    }
  }
  return sResolveOne(h, root, 0, stack, 0);
}

/*!
 * @brief Reads @p tok as a canonical decimal array index.
 *
 * Canonical only: every character a digit, no sign, and no leading zero except
 * for "0" itself.  The sibling scan this replaces compares the token against
 * child names with @c strcmp, so "007" and "+7" never matched the child named
 * "7" — a looser numeric parse here would take a misspelled index and resolve
 * it silently to a real element.
 *
 * @return true when @p tok is canonical, with the value in @p out.
 */
static bool sParseArrayIndex(const char *tok, size_t *out)
{
  if (!tok || !tok[0]) return false;
  if (tok[0] == '0' && tok[1] != '\0') return false;
  size_t v = 0;
  for (const char *c = tok; *c; c++) {
    if (*c < '0' || *c > '9') return false;
    size_t next = v * 10 + (size_t)(*c - '0');
    if (next < v) return false; /* would wrap */
    v = next;
  }
  *out = v;
  return true;
}

UfNode *ConfigFindPath(UfNode *root, const char *path)
{
  if (!root || !path) return NULL;
  if (path[0] == 0) return root;
  if (strlen(path) >= CONFIG_DEFAULT_UFCONFIG_PATH_MAX) return NULL;
  char buf[CONFIG_DEFAULT_UFCONFIG_PATH_MAX];
  memcpy(buf, path, strlen(path) + 1);
  UfNode *cur  = root;
  char *  save = NULL;
  char *  tok  = strtok_r(buf, ".", &save);
  while (tok) {
    UfNode *found = NULL;
    if (cur->kind == UF_NODE_ALIAS && cur->resolved) cur = cur->resolved;

    /* An array holds its children in index order, so the token can be used as
       a position instead of being compared against every sibling name.  The
       node the walk lands on is still checked against the token: UfConfigUnset
       removes a child without renumbering or decrementing array_count, so
       position and name can disagree, and a positional jump that trusted the
       position alone would return a different field's value with no error.
       When the check fails the scan below runs and decides, so this path can
       only ever be faster, never different. */
    if (cur->kind == UF_NODE_ARRAY) {
      size_t idx = 0;
      if (sParseArrayIndex(tok, &idx) && idx < cur->array_count) {
        UfNode *c = cur->first_child;
        for (size_t i = 0; c && i < idx; i++) c = c->next_sibling;
        if (c && c->name && strcmp(c->name, tok) == 0) found = c;
      }
    }

    if (!found) for (UfNode *c = cur->first_child; c; c = c->next_sibling) {
      if (c->name && strcmp(c->name, "#bindings") == 0) {
        for (UfNode *b = c->first_child; b; b = b->next_sibling) if (b->name && strcmp(b->name, tok) == 0) {
          found = b;
          break;
        }
      }
      if (c->name && strcmp(c->name, tok) == 0) {
        found = c;
        break;
      }
    }
    if (!found) return NULL;
    cur = found;
    tok = strtok_r(NULL, ".", &save);
  }
  return cur;
}

UfConfigStatus ConfigNodeToValue(const UfNode *n, UfConfigValue *out)
{
  memset(out, 0, sizeof(*out));
  if (!n) {
    out->kind    = UF_CONFIG_KIND_ABSENT;
    out->present = false;
    return UF_CONFIG_ERR_NOFIELD;
  }
  out->present = true;
  out->desc    = n->desc;
  if (n->kind == UF_NODE_ARRAY) {
    out->kind           = UF_CONFIG_KIND_ARRAY;
    out->as.array.elems = NULL;
    out->as.array.count = n->array_count;
    return UF_CONFIG_OK;
  }
  if (n->kind == UF_NODE_ALIAS || n->kind == UF_NODE_SCOPE) {
    out->kind = UF_CONFIG_KIND_SCOPE;
    return UF_CONFIG_OK;
  }
  out->kind = n->vkind;
  switch (n->vkind) {
  case UF_CONFIG_KIND_INT:
    out->as.i = n->v.i.eff;
    out->raw.i = n->v.i.raw;
    break;
  case UF_CONFIG_KIND_FLOAT:
    out->as.f = n->v.f.eff;
    out->raw.f = n->v.f.raw;
    break;
  case UF_CONFIG_KIND_BOOL:
    out->as.b = n->v.b.eff;
    out->raw.b = n->v.b.raw;
    break;
  case UF_CONFIG_KIND_STRING:
    out->as.str.ptr = n->v.s.eff;
    out->as.str.len  = n->v.s.eff_len;
    out->raw.str.ptr = n->v.s.raw;
    out->raw.str.len = n->v.s.raw_len;
    break;
  case UF_CONFIG_KIND_BYTES:
    out->as.bytes.ptr = (const uint8_t*)n->v.s.eff;
    out->as.bytes.len = n->v.s.eff_len;
    break;
  default: break;
  }
  return UF_CONFIG_OK;
}
