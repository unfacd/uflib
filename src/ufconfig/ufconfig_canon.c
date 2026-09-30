/**
 * @file ufconfig_canon.c
 * @brief Canonical form, the subtree digest built from it, and the four serialisers.
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
  char * b;
  size_t n, cap;
} Buf;

static int sBputc(Buf *b, char c)
{
  if (b->n + 1 >= b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 256;
    char * p  = (char*)realloc(b->b, nc);
    if (!p) return -1;
    b->b   = p;
    b->cap = nc;
  }
  b->b[b->n++] = c;
  return 0;
}

static int sBputs(Buf *b, const char *s)
{
  while (*s) if (sBputc(b, *s++)) return -1;
  return 0;
}

static int sEmitStr(Buf *b, const char *s, size_t n)
{
  if (sBputc(b, '"')) return -1;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      sBputc(b, '\\');
      sBputc(b, (char)c);
    }
    else if (c == '\n') sBputs(b, "\\n");
    else if (c == '\r') sBputs(b, "\\r");
    else if (c == '\t') sBputs(b, "\\t");
    else if (c < 0x20) {
      char t[8];
      snprintf(t, sizeof(t), "\\u%04x", c);
      sBputs(b, t);
    }
    else if (sBputc(b, (char)c)) return -1;
  }
  return sBputc(b, '"');
}

static int sCmpName(const void *a, const void *b)
{
  const UfNode *const *pa = a, *const *pb = b;
  return strcmp((*pa)->name ? (*pa)->name : "", (*pb)->name ? (*pb)->name : "");
}

static int sCanonNode(const UfNode *n, Buf *b);

/*!
 * @brief Number of children of @p src that belong in a scope body.
 *
 * The @c #bindings holder is skipped: its contents are spliced into the parent
 * elsewhere and it is not part of the emitted table.
 */
static size_t sScopeChildCount(const UfNode *src)
{
  size_t k = 0;
  for (const UfNode *c = src->first_child; c; c = c->next_sibling)
    if (!(c->name && strcmp(c->name, "#bindings") == 0)) k++;
  return k;
}

/*!
 * @brief Emits @p n as a canonical table.
 *
 * Children are collected onto the stack up to @ref CANON_STACK_KIDS and
 * allocated above it.  A fixed 256-slot stack array was the previous shape, and
 * it **silently dropped every child past the 256th** — out of this emitter, and
 * therefore out of the digest — while INI, YAML, Lua and the namespace walk all
 * still reported them.  A scope wider than that made the digest disagree with
 * every other serialiser, with no error.  Nothing in the corpus has a scope
 * wider than 34, which is why it was never seen.
 */
static int sCanonScope(const UfNode *n, Buf *b)
{
  const UfNode *src   = (n->kind == UF_NODE_ALIAS && n->resolved) ? n->resolved : n;
  size_t        count = sScopeChildCount(src);

  const UfNode *stack_kids[PRIV_CONFIG_DEFAULT_UFCONFIG_CANON_STACK_KIDS];
  const UfNode **kids = (count > PRIV_CONFIG_DEFAULT_UFCONFIG_CANON_STACK_KIDS)
                          ? (const UfNode **)malloc(count * sizeof(const UfNode *))
                          : stack_kids;
  if (!kids) return -1;

  size_t nk = 0;
  for (const UfNode *c = src->first_child; c; c = c->next_sibling)
    if (!(c->name && strcmp(c->name, "#bindings") == 0)) kids[nk++] = c;
  qsort(kids, nk, sizeof(const UfNode *), sCmpName);

  int rc = sBputc(b, '{');
  for (size_t i = 0; rc == 0 && i < nk; i++) {
    if (i) rc = sBputc(b, ',');
    if (rc == 0) sEmitStr(b, kids[i]->name ? kids[i]->name : "", kids[i]->name ? strlen(kids[i]->name) : 0);
    if (rc == 0) rc = sBputc(b, ':');
    if (rc == 0) rc = sCanonNode(kids[i], b);
  }
  if (rc == 0) rc = sBputc(b, '}');

  if (kids != stack_kids) free(kids);
  return rc;
}

static int sCanonArray(const UfNode *n, Buf *b)
{
  if (sBputc(b, '[')) return -1;
  int first = 1;
  for (UfNode *c = n->first_child; c; c = c->next_sibling) {
    if (!first && sBputc(b, ',')) return -1;
    first = 0;
    if (sCanonNode(c, b)) return -1;
  }
  return sBputc(b, ']');
}

static int sCanonNode(const UfNode *n, Buf *b)
{
  if (!n) return sBputs(b, "{}");
  if (n->kind == UF_NODE_ARRAY) return sCanonArray(n, b);
  if (n->kind == UF_NODE_ALIAS || n->kind == UF_NODE_SCOPE) return sCanonScope(n, b);
  switch (n->vkind) {
  case UF_CONFIG_KIND_INT: {
    char t[64];
    snprintf(t, sizeof(t), "%lld", (long long)n->v.i.raw);
    return sBputs(b, t);
  }
  case UF_CONFIG_KIND_FLOAT: {
    char t[64];
    snprintf(t, sizeof(t), "%.17g", n->v.f.raw);
    if (!strchr(t, '.') && !strchr(t, 'e') && !strchr(t, 'E')) strcat(t, ".0");
    return sBputs(b, t);
  }
  case UF_CONFIG_KIND_BOOL: return sBputs(b, n->v.b.raw ? "true" : "false");
  case UF_CONFIG_KIND_STRING: return sEmitStr(b, n->v.s.raw ? n->v.s.raw : "", n->v.s.raw_len);
  default: return sBputs(b, "null");
  }
}

static void sDigestTree(UfNode *n)
{
  if (!n) return;
  Buf b = {0};
  if (!sCanonNode(n, &b)) ConfigSha256(b.b, b.n, n->digest);
  free(b.b);
  for (UfNode *c = n->first_child; c; c = c->next_sibling) sDigestTree(c);
}

UfConfigStatus ConfigCanonAndDigest(UfNode *n, unsigned char out[32])
{
  Buf b = {0};
  if (sCanonNode(n, &b)) {
    free(b.b);
    return UF_CONFIG_ERR_NO_MEMORY;
  }
  UfConfigStatus st = ConfigSha256(b.b, b.n, out);
  if (st == UF_CONFIG_OK && n) {
    memcpy(n->digest, out, 32);
    for (UfNode *c = n->first_child; c; c = c->next_sibling) sDigestTree(c);
  }
  free(b.b);
  return st;
}

static UfConfigStatus sFillOrNeed(char *buf, size_t cap, size_t *needed, const char *s, size_t n)
{
  *needed = n + 1;
  if (!buf || cap == 0) return UF_CONFIG_OK;
  size_t cpy = n < cap ? n : (cap ? cap - 1 : 0);
  if (buf && cap) {
    memcpy(buf, s, cpy);
    buf[cpy < cap ? cpy : cap - 1] = 0;
  }
  return UF_CONFIG_OK;
}

UfConfigStatus ConfigEmitJson(const UfNode *n, char *buf, size_t cap, size_t *needed, bool pretty, bool expand)
{
  (void)pretty;
  (void)expand;
  Buf b = {0};
  if (sCanonNode(n, &b)) {
    free(b.b);
    return UF_CONFIG_ERR_NO_MEMORY;
  }
  UfConfigStatus st = sFillOrNeed(buf, cap, needed, b.b, b.n);
  free(b.b);
  return st;
}

static int sYamlNeedQuote(const char *s)
{
  if (!s || !s[0]) return 1;
  if (!strcmp(s, "true") || !strcmp(s, "false") || !strcmp(s, "null") || !strcmp(s, "yes") || !strcmp(s, "no") || !
    strcmp(s, "~") || s[0] == '#' || ConfigAsciiIsDigit((unsigned char)s[0])) return 1;
  return 0;
}

/* One YAML scalar.  Shared so the mapping path and the sequence path cannot
   drift: a float that is a float in one and null in the other is the defect
   this exists to prevent. */
static void sYamlScalar(const UfNode *n, Buf *b)
{
  char t[64];
  switch (n->vkind) {
  case UF_CONFIG_KIND_INT:
    snprintf(t, sizeof(t), "%lld", (long long)n->v.i.raw);
    sBputs(b, t);
    break;
  case UF_CONFIG_KIND_FLOAT:
    /* Was reaching the null fallback: a float has no integer form and no
       quotes, so nothing claimed it and every float serialised as null. */
    snprintf(t, sizeof(t), "%.17g", n->v.f.raw);
    sBputs(b, t);
    break;
  case UF_CONFIG_KIND_BOOL:
    sBputs(b, n->v.b.raw ? "true" : "false");
    break;
  case UF_CONFIG_KIND_STRING: {
    const char *v = n->v.s.raw ? n->v.s.raw : "";
    if (sYamlNeedQuote(v) || strchr(v, ' ')) sEmitStr(b, v, n->v.s.raw_len);
    else sBputs(b, v);
    break;
  }
  default: sBputs(b, "null"); break;
  }
}

static int sYamlScope(const UfNode *n, Buf *b, int indent)
{
  const UfNode *src = (n->kind == UF_NODE_ALIAS && n->resolved) ? n->resolved : n;
  for (UfNode *c = src->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) continue;
    for (int i = 0; i < indent; i++) sBputc(b, ' ');
    sBputs(b, c->name ? c->name : "?");
    sBputs(b, ":");
    if (c->kind == UF_NODE_ARRAY) {
      /* A block sequence.  Falling through to the scalar branch below emitted
         the node's count instead -- `list_v: 0` -- which parses and loses every
         element. */
      sBputc(b, '\n');
      for (UfNode *e = c->first_child; e; e = e->next_sibling) {
        for (int i = 0; i < indent + 2; i++) sBputc(b, ' ');
        sBputs(b, "- ");
        sYamlScalar(e, b);
        sBputc(b, '\n');
      }
      continue;
    }
    if (c->kind == UF_NODE_SCOPE || c->kind == UF_NODE_ALIAS) {
      sBputc(b, '\n');
      sYamlScope(c, b, indent + 2);
    }
    else {
      sBputc(b, ' ');
      sYamlScalar(c, b);
      sBputc(b, '\n');
    }
  }
  return 0;
}

UfConfigStatus ConfigEmitYaml(const UfNode *n, char *buf, size_t cap, size_t *needed, bool pretty)
{
  (void)pretty;
  Buf b = {0};
  sYamlScope(n, &b, 0);
  UfConfigStatus st = sFillOrNeed(buf, cap, needed, b.b ? b.b : "", b.n);
  free(b.b);
  return st;
}

static int sIniWalk(const UfNode *n, const char *pfx, Buf *b)
{
  const UfNode *src = (n->kind == UF_NODE_ALIAS && n->resolved) ? n->resolved : n;
  if (n->kind == UF_NODE_LEAF) {
    sBputs(b, pfx);
    sBputs(b, " = ");
    if (n->vkind == UF_CONFIG_KIND_STRING) sEmitStr(b, n->v.s.raw ? n->v.s.raw : "", n->v.s.raw_len);
    else if (n->vkind == UF_CONFIG_KIND_INT) {
      char t[64];
      snprintf(t, sizeof(t), "%lld", (long long)n->v.i.raw);
      sBputs(b, t);
    }
    else if (n->vkind == UF_CONFIG_KIND_BOOL) sBputs(b, n->v.b.raw ? "true" : "false");
    else if (n->vkind == UF_CONFIG_KIND_FLOAT) {
      /* Was emitting nothing after the ' = ', so the line read as an empty
         string value rather than as absent. */
      char t[64];
      snprintf(t, sizeof(t), "%.17g", n->v.f.raw);
      sBputs(b, t);
    }
    sBputc(b, '\n');
    return 0;
  }
  for (UfNode *c = src->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) continue;
    char np[512];
    if (pfx && pfx[0]) snprintf(np, sizeof(np), "%s.%s", pfx, c->name);
    else snprintf(np, sizeof(np), "%s", c->name ? c->name : "");
    sIniWalk(c, np, b);
  }
  return 0;
}

UfConfigStatus ConfigEmitIni(const UfNode *n, char *buf, size_t cap, size_t *needed)
{
  Buf b = {0};
  sIniWalk(n, "", &b);
  UfConfigStatus st = sFillOrNeed(buf, cap, needed, b.b ? b.b : "", b.n);
  free(b.b);
  return st;
}

static int sLuaVal(const UfNode *n, Buf *b, int indent);

static int sLuaScope(const UfNode *n, Buf *b, int indent)
{
  sBputs(b, "{\n");
  for (UfNode *c = n->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) continue;
    for (int i = 0; i < indent + 4; i++) sBputc(b, ' ');
    sBputs(b, c->name ? c->name : "_");
    sBputs(b, " = ");
    sLuaVal(c, b, indent + 4);
    sBputs(b, ",\n");
  }
  for (int i = 0; i < indent; i++) sBputc(b, ' ');
  sBputc(b, '}');
  return 0;
}

static int sLuaVal(const UfNode *n, Buf *b, int indent)
{
  if (n->kind == UF_NODE_ALIAS) {
    sBputs(b, n->alias_target ? n->alias_target : "nil");
    return 0;
  }
  if (n->kind == UF_NODE_SCOPE) return sLuaScope(n, b, indent);
  if (n->kind == UF_NODE_ARRAY) {
    /* A table of values.  The scalar fallthrough emitted the array's count --
       `list_v = 0` -- which reads back as one integer and loses every element. */
    if (sBputs(b, "{")) return -1;
    int first = 1;
    for (UfNode *e = n->first_child; e; e = e->next_sibling) {
      if (!first && sBputc(b, ',')) return -1;
      first = 0;
      if (sBputc(b, '\n')) return -1;
      for (int i = 0; i <= indent + 4; i++) sBputc(b, ' ');
      if (sLuaVal(e, b, indent + 4)) return -1;
    }
    if (!first) {
      if (sBputc(b, '\n')) return -1;
      for (int i = 0; i < indent + 4; i++) sBputc(b, ' ');
    }
    return sBputc(b, '}');
  }
  switch (n->vkind) {
  case UF_CONFIG_KIND_INT: {
    char t[64];
    snprintf(t, sizeof(t), "%lld", (long long)n->v.i.raw);
    return sBputs(b, t);
  }
  case UF_CONFIG_KIND_FLOAT: {
    /* Was reaching the nil default, so every float round-tripped as absent. */
    char t[64];
    snprintf(t, sizeof(t), "%.17g", n->v.f.raw);
    if (!strchr(t, '.') && !strchr(t, 'e') && !strchr(t, 'E')) strcat(t, ".0");
    return sBputs(b, t);
  }
  case UF_CONFIG_KIND_BOOL: return sBputs(b, n->v.b.raw ? "true" : "false");
  case UF_CONFIG_KIND_STRING: return sEmitStr(b, n->v.s.raw ? n->v.s.raw : "", n->v.s.raw_len);
  default: return sBputs(b, "nil");
  }
}

UfConfigStatus ConfigEmitLua(const UfNode *n, char *buf, size_t cap, size_t *needed)
{
  Buf     b    = {0};
  UfNode *hold = NULL;
  for (UfNode *c = n->first_child; c; c = c->next_sibling) if (c->name && strcmp(c->name, "#bindings") == 0) hold = c;
  if (hold) {
    for (UfNode *c = hold->first_child; c; c = c->next_sibling) {
      sBputs(&b, c->name ? c->name : "_");
      sBputs(&b, " = ");
      sLuaVal(c, &b, 0);
      sBputs(&b, "\n\n");
    }
    sBputs(&b, "return ");
    sLuaScope(n, &b, 0);
    sBputc(&b, '\n');
  }
  else {
    sLuaScope(n, &b, 0);
    sBputc(&b, '\n');
  }
  UfConfigStatus st = sFillOrNeed(buf, cap, needed, b.b ? b.b : "", b.n);
  free(b.b);
  return st;
}

/*!
 * @brief Emits @p h without taking the lock; the public entry points take it.
 *
 * The lock is held by the callers rather than here so that the allocate-and-
 * fill pair brackets both passes.  Sizing from one tree and filling from
 * another would write past the buffer the size was computed for -- and a
 * reload between the two calls is exactly that.
 *
 * Not taking it here also matters because this is where the tree is walked:
 * with no lock at all, a reload could release the arena underneath the walk,
 * which is a use-after-free on a path the header advertises as thread-safe.
 */
static UfConfigStatus sToJson(const UfConfig *h, char *buf, size_t cap, size_t *needed)
{
  if (!h->root) return UF_CONFIG_ERR_NOFIELD;
  return ConfigEmitJson(h->root, buf, cap, needed, false, true);
}

PUBLIC_API UfConfigStatus UfConfigToJson(const UfConfig *h, char *buf, size_t cap, size_t *needed)
{
  if (!h || !needed) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  UfConfigStatus st = sToJson(h, buf, cap, needed);
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return st;
}

PUBLIC_API UfConfigStatus UfConfigToJsonAlloc(const UfConfig *h, char **out)
{
  if (!h || !out) return UF_CONFIG_ERR_INVALID_ARG;
  *out = NULL;
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  size_t         n  = 0;
  UfConfigStatus st = sToJson(h, NULL, 0, &n);
  char *         p  = NULL;
  if (st == UF_CONFIG_OK) {
    p = (char*)malloc(n ? n : 1);
    if (!p) st = UF_CONFIG_ERR_NO_MEMORY;
    else {
      st = sToJson(h, p, n, &n);
      /* The two passes are only meaningful together; a fill that failed leaves
         nothing to hand back, and the caller owns nothing it did not get. */
      if (st == UF_CONFIG_OK) *out = p;
      else free(p);
    }
  }
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return st;
}

/*!
 * @brief Emits @p h without taking the lock; the public entry points take it.
 *
 * The lock is held by the callers rather than here so that the allocate-and-
 * fill pair brackets both passes.  Sizing from one tree and filling from
 * another would write past the buffer the size was computed for -- and a
 * reload between the two calls is exactly that.
 *
 * Not taking it here also matters because this is where the tree is walked:
 * with no lock at all, a reload could release the arena underneath the walk,
 * which is a use-after-free on a path the header advertises as thread-safe.
 */
static UfConfigStatus sToYaml(const UfConfig *h, char *buf, size_t cap, size_t *needed)
{
  if (!h->root) return UF_CONFIG_ERR_NOFIELD;
  return ConfigEmitYaml(h->root, buf, cap, needed, true);
}

PUBLIC_API UfConfigStatus UfConfigToYaml(const UfConfig *h, char *buf, size_t cap, size_t *needed)
{
  if (!h || !needed) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  UfConfigStatus st = sToYaml(h, buf, cap, needed);
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return st;
}

PUBLIC_API UfConfigStatus UfConfigToYamlAlloc(const UfConfig *h, char **out)
{
  if (!h || !out) return UF_CONFIG_ERR_INVALID_ARG;
  *out = NULL;
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  size_t         n  = 0;
  UfConfigStatus st = sToYaml(h, NULL, 0, &n);
  char *         p  = NULL;
  if (st == UF_CONFIG_OK) {
    p = (char*)malloc(n ? n : 1);
    if (!p) st = UF_CONFIG_ERR_NO_MEMORY;
    else {
      st = sToYaml(h, p, n, &n);
      /* The two passes are only meaningful together; a fill that failed leaves
         nothing to hand back, and the caller owns nothing it did not get. */
      if (st == UF_CONFIG_OK) *out = p;
      else free(p);
    }
  }
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return st;
}

/*!
 * @brief Emits @p h without taking the lock; the public entry points take it.
 *
 * The lock is held by the callers rather than here so that the allocate-and-
 * fill pair brackets both passes.  Sizing from one tree and filling from
 * another would write past the buffer the size was computed for -- and a
 * reload between the two calls is exactly that.
 *
 * Not taking it here also matters because this is where the tree is walked:
 * with no lock at all, a reload could release the arena underneath the walk,
 * which is a use-after-free on a path the header advertises as thread-safe.
 */
static UfConfigStatus sToIni(const UfConfig *h, char *buf, size_t cap, size_t *needed)
{
  if (!h->root) return UF_CONFIG_ERR_NOFIELD;
  return ConfigEmitIni(h->root, buf, cap, needed);
}

PUBLIC_API UfConfigStatus UfConfigToIni(const UfConfig *h, char *buf, size_t cap, size_t *needed)
{
  if (!h || !needed) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  UfConfigStatus st = sToIni(h, buf, cap, needed);
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return st;
}

PUBLIC_API UfConfigStatus UfConfigToIniAlloc(const UfConfig *h, char **out)
{
  if (!h || !out) return UF_CONFIG_ERR_INVALID_ARG;
  *out = NULL;
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  size_t         n  = 0;
  UfConfigStatus st = sToIni(h, NULL, 0, &n);
  char *         p  = NULL;
  if (st == UF_CONFIG_OK) {
    p = (char*)malloc(n ? n : 1);
    if (!p) st = UF_CONFIG_ERR_NO_MEMORY;
    else {
      st = sToIni(h, p, n, &n);
      /* The two passes are only meaningful together; a fill that failed leaves
         nothing to hand back, and the caller owns nothing it did not get. */
      if (st == UF_CONFIG_OK) *out = p;
      else free(p);
    }
  }
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return st;
}

/*!
 * @brief Emits @p h without taking the lock; the public entry points take it.
 *
 * The lock is held by the callers rather than here so that the allocate-and-
 * fill pair brackets both passes.  Sizing from one tree and filling from
 * another would write past the buffer the size was computed for -- and a
 * reload between the two calls is exactly that.
 *
 * Not taking it here also matters because this is where the tree is walked:
 * with no lock at all, a reload could release the arena underneath the walk,
 * which is a use-after-free on a path the header advertises as thread-safe.
 */
static UfConfigStatus sToLuaStyle(const UfConfig *h, char *buf, size_t cap, size_t *needed)
{
  if (!h->root) return UF_CONFIG_ERR_NOFIELD;
  return ConfigEmitLua(h->root, buf, cap, needed);
}

PUBLIC_API UfConfigStatus UfConfigToLuaStyle(const UfConfig *h, char *buf, size_t cap, size_t *needed)
{
  if (!h || !needed) return UF_CONFIG_ERR_INVALID_ARG;
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  UfConfigStatus st = sToLuaStyle(h, buf, cap, needed);
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return st;
}

PUBLIC_API UfConfigStatus UfConfigToLuaStyleAlloc(const UfConfig *h, char **out)
{
  if (!h || !out) return UF_CONFIG_ERR_INVALID_ARG;
  *out = NULL;
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  size_t         n  = 0;
  UfConfigStatus st = sToLuaStyle(h, NULL, 0, &n);
  char *         p  = NULL;
  if (st == UF_CONFIG_OK) {
    p = (char*)malloc(n ? n : 1);
    if (!p) st = UF_CONFIG_ERR_NO_MEMORY;
    else {
      st = sToLuaStyle(h, p, n, &n);
      /* The two passes are only meaningful together; a fill that failed leaves
         nothing to hand back, and the caller owns nothing it did not get. */
      if (st == UF_CONFIG_OK) *out = p;
      else free(p);
    }
  }
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return st;
}
