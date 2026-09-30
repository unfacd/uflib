/**
 * @file ufconfig_pairs.c
 * @brief The pair-set form: flattening a handle for a store, and building one back.
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

static char *sEncDup(const UfNode *n)
{
  char buf[512];
  if (n->vkind == UF_CONFIG_KIND_INT) {
    snprintf(buf, sizeof(buf), "%lld", (long long)n->v.i.raw);
    return strdup(buf);
  }
  if (n->vkind == UF_CONFIG_KIND_FLOAT) {
    snprintf(buf, sizeof(buf), "%.17g", n->v.f.raw);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) strcat(buf, ".0");
    return strdup(buf);
  }
  if (n->vkind == UF_CONFIG_KIND_BOOL) return strdup(n->v.b.raw ? "true" : "false");
  if (n->vkind == UF_CONFIG_KIND_STRING) {
    size_t nlen = n->v.s.raw_len;
    char * o    = (char*)malloc(nlen + 3);
    o[0]        = '"';
    memcpy(o + 1, n->v.s.raw ? n->v.s.raw : "", nlen);
    o[1 + nlen] = '"';
    o[2 + nlen] = 0;
    return o;
  }
  return strdup("null");
}

static size_t sCountLeaves(const UfNode *n)
{
  if (!n) return 0;
  size_t k = 0;
  if (n->kind == UF_NODE_LEAF && n->path && n->path[0]) k = 1;
  if ((n->kind == UF_NODE_SCOPE || n->kind == UF_NODE_ARRAY) && !n->first_child && n->path && n->path[0]) k = 1;
  /* empty table */
  /* An alias's contents live on the node it resolved to, not on itself, so
     counting its own children would miss the whole subtree. */
  if (n->kind == UF_NODE_ALIAS && n->resolved) return k + sCountLeaves(n->resolved);
  for (UfNode *c = n->first_child; c; c = c->next_sibling) k += sCountLeaves(c);
  return k;
}

/* Emit an aliased subtree under the *alias's* path.
 *
 * The leaves of a resolved table carry the path of where the table is defined
 * -- `shared_timeouts.connected` -- but a consumer reads it where the alias
 * reaches it -- `nest.shared.connected`.  Emitting the target's own paths would
 * publish a name nobody asked for and lose the one they did, so the alias's
 * path is substituted for the target's prefix here. */
static void sCollectAliased(const UfNode *alias, const UfNode *n,
                            const char *target_prefix, UfConfigFieldPair *out, size_t *k)
{
  if (!n) return;
  if (n->kind == UF_NODE_LEAF && n->path && n->path[0]) {
    const char *suffix = n->path;
    size_t      plen   = strlen(target_prefix);
    if (plen && strncmp(n->path, target_prefix, plen) == 0) {
      suffix = n->path + plen;
      if (*suffix == '.') suffix++;
    }
    size_t need = strlen(alias->path) + 1 + strlen(suffix) + 1;
    char  *p    = (char *)malloc(need);
    if (p) {
      snprintf(p, need, "%s.%s", alias->path, suffix);
      out[*k].path    = p;
      out[*k].encoded = sEncDup(n);
      if (n->vkind == UF_CONFIG_KIND_INT) out[*k].vtype = UF_PAIR_INT;
      else if (n->vkind == UF_CONFIG_KIND_FLOAT) out[*k].vtype = UF_PAIR_FLOAT;
      else if (n->vkind == UF_CONFIG_KIND_BOOL) out[*k].vtype = UF_PAIR_BOOL;
      else out[*k].vtype = UF_PAIR_STRING;
      (*k)++;
    }
  }
  for (UfNode *c = n->first_child; c; c = c->next_sibling)
    sCollectAliased(alias, c, target_prefix, out, k);
}

static void sCollect(const UfNode *n, UfConfigFieldPair *out, size_t *k)
{
  if (!n) return;
  if (n->name && strcmp(n->name, "#bindings") == 0) {
    for (UfNode *c = n->first_child; c; c = c->next_sibling) sCollect(c, out, k);
    return;
  }
  if (n->kind == UF_NODE_LEAF && n->path && n->path[0]) {
    out[*k].path    = strdup(n->path);
    out[*k].encoded = sEncDup(n);
    if (n->vkind == UF_CONFIG_KIND_INT) out[*k].vtype = UF_PAIR_INT;
    else if (n->vkind == UF_CONFIG_KIND_FLOAT) out[*k].vtype = UF_PAIR_FLOAT;
    else if (n->vkind == UF_CONFIG_KIND_BOOL) out[*k].vtype = UF_PAIR_BOOL;
    else out[*k].vtype                                      = UF_PAIR_STRING;
    (*k)++;
  }
  else if ((n->kind == UF_NODE_SCOPE || n->kind == UF_NODE_ARRAY) && !n->first_child && n->path && n->path[0]) {
    out[*k].path    = strdup(n->path);
    out[*k].encoded = strdup("{}");
    out[*k].vtype   = UF_PAIR_EMPTY_TABLE;
    (*k)++;
  }
  /* An alias has no children of its own; its contents are the resolved
     node's, and they belong under this alias's path. */
  if (n->kind == UF_NODE_ALIAS && n->resolved && n->path && n->path[0]) {
    sCollectAliased(n, n->resolved, n->resolved->path ? n->resolved->path : "", out, k);
    return;
  }
  for (UfNode *c = n->first_child; c; c = c->next_sibling) sCollect(c, out, k);
}

PUBLIC_API UfConfigStatus UfConfigFlattenHandle(const UfConfig *h, UfConfigFieldPair **out, size_t *np,
                                                UfConfigMeta *  meta)
{
  if (!h || !out || !np) return UF_CONFIG_ERR_INVALID_ARG;
  size_t             cap = sCountLeaves(h->root);
  UfConfigFieldPair *a   = (UfConfigFieldPair*)calloc(cap ? cap : 1, sizeof(*a));
  size_t             k   = 0;
  sCollect(h->root, a, &k);
  *out = a;
  *np  = k;
  if (meta) {
    memset(meta, 0, sizeof(*meta));
    meta->fmt               = 1;
    meta->version           = 1;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
      meta->digest_hex[i * 2]     = hex[h->digest[i] >> 4];
      meta->digest_hex[i * 2 + 1] = hex[h->digest[i] & 0xF];
    }
    meta->is_digest_set = 1;
  }
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigPairsFromLua(const char *buf, size_t n, UfConfigFieldPair **out, size_t *np)
{
  UfConfig *h = NULL;
  ConfigCreate(&h);
  UfConfigLoadOptions opt = {.size = sizeof(opt), .version = 1, .mode = UF_CONFIG_LOAD_LENIENT};
  UfConfigStatus      st  = UfConfigLoadBuffer(h, buf, n, &opt, NULL);
  if (st && st != UF_CONFIG_ERR_REQUIRED && st != UF_CONFIG_ERR_UNKNOWN_FIELD) {
    UfConfigDestroy(h);
    return st;
  }
  st = UfConfigFlattenHandle(h, out, np, NULL);
  UfConfigDestroy(h);
  return st;
}

static int sIsIndexName(const char *s)
{
  if (!s || !*s) return 0;
  if (s[0] == '0') return s[1] == 0;
  for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
  return 1;
}

static UfConfigStatus sMarkArrays(UfNode *n)
{
  if (!n) return UF_CONFIG_OK;
  for (UfNode *c = n->first_child; c; c = c->next_sibling) {
    UfConfigStatus st = sMarkArrays(c);
    if (st) return st;
  }
  int           nchild = 0, numeric = 0, maxix = -1;
  unsigned char seen[512];
  memset(seen, 0, sizeof(seen));
  for (UfNode *c = n->first_child; c; c = c->next_sibling) {
    nchild++;
    if (!sIsIndexName(c->name)) continue;
    int ix = atoi(c->name);
    if (ix < 0 || ix >= 512) return UF_CONFIG_ERR_PARSE;
    if (seen[ix]) return UF_CONFIG_ERR_PARSE;
    seen[ix] = 1;
    numeric++;
    if (ix > maxix) maxix = ix;
  }
  if (numeric == 0) return UF_CONFIG_OK;
  if (numeric != nchild) return UF_CONFIG_ERR_PARSE;
  if (maxix + 1 != nchild) return UF_CONFIG_ERR_PARSE;
  /* order children 0..n-1 */
  UfNode *ord[512];
  memset(ord, 0, sizeof(ord));
  for (UfNode *c = n->first_child; c; c = c->next_sibling) ord[atoi(c->name)] = c;
  n->first_child = NULL;
  UfNode *tail   = NULL;
  for (int i = 0; i < nchild; i++) {
    ord[i]->next_sibling = NULL;
    if (!n->first_child) n->first_child = ord[i];
    else tail->next_sibling             = ord[i];
    tail = ord[i];
  }
  n->kind        = UF_NODE_ARRAY;
  n->array_count = (size_t)nchild;
  return UF_CONFIG_OK;
}

static UfNode *sEnsurePath(UfConfig *h, UfNode *root, const char *path)
{
  char tmp[512];
  if (strlen(path) >= sizeof(tmp)) return NULL;
  memcpy(tmp, path, strlen(path) + 1);
  UfNode *cur  = root;
  char *  save = NULL;
  for (char *tok = strtok_r(tmp, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
    UfNode *found = NULL;
    for (UfNode *c = cur->first_child; c; c = c->next_sibling) if (c->name && strcmp(c->name, tok) == 0) {
      found = c;
      break;
    }
    if (!found) {
      found = (UfNode*)ConfigArenaAlloc(&h->arena, sizeof(UfNode));
      memset(found, 0, sizeof(*found));
      found->kind         = UF_NODE_SCOPE;
      found->name         = ConfigArenaStrndup(&h->arena, tok, strlen(tok));
      found->parent       = cur;
      found->next_sibling = cur->first_child;
      cur->first_child    = found;
    }
    cur = found;
  }
  return cur;
}

PUBLIC_API UfConfigStatus UfConfigLoadPairs(UfConfig *          h, const UfConfigFieldPair *     pairs, size_t n,
                                            const UfConfigMeta *meta, const UfConfigLoadOptions *opt,
                                            UfConfigLoadReport *report)
{
  if (!h) return UF_CONFIG_ERR_INVALID_ARG;
  (void)meta;
  /* The build lock is taken for the whole build; the rwlock only at the
   * publish below.
   *
   * The build is exclusive against other writers because they contend for
   * @c arena -- every writer allocates, and no reader does -- and it needs no
   * exclusion from readers at all: it works in an arena of its own and leaves
   * @c root, @c by_id and @c unlisted alone until the swap.  Releasing the
   * previous arena here was the other half of that exclusion, and was unsound
   * besides: readers may still be walking it.  Retirement happens at the
   * publish, where the reader count is meaningful. */
  pthread_mutex_lock(&h->build_lock);
  UfArena *     old_a = h->arena;
  unsigned char new_d[32];
  memset(new_d, 0, sizeof(new_d));

  h->arena = NULL; /* the build allocates here; only a writer ever looks */
  UfNode *root = (UfNode*)ConfigArenaAlloc(&h->arena, sizeof(UfNode));
  memset(root, 0, sizeof(*root));
  root->kind = UF_NODE_SCOPE;
  root->name = ConfigArenaStrndup(&h->arena, "", 0);
  for (size_t i = 0; i < n; i++) {
    if (!pairs[i].path) continue;
    UfNode *leaf = sEnsurePath(h, root, pairs[i].path);
    if (!leaf) continue;
    if (pairs[i].vtype == UF_PAIR_EMPTY_TABLE) {
      leaf->kind = UF_NODE_SCOPE;
      continue;
    }
    leaf->kind    = UF_NODE_LEAF;
    const char *e = pairs[i].encoded ? pairs[i].encoded : "";
    if (pairs[i].vtype == UF_PAIR_INT) {
      leaf->vkind = UF_CONFIG_KIND_INT;
      leaf->v.i.raw = leaf->v.i.eff = atoll(e);
    }
    else if (pairs[i].vtype == UF_PAIR_FLOAT) {
      leaf->vkind = UF_CONFIG_KIND_FLOAT;
      leaf->v.f.raw = leaf->v.f.eff = atof(e);
    }
    else if (pairs[i].vtype == UF_PAIR_BOOL) {
      leaf->vkind = UF_CONFIG_KIND_BOOL;
      leaf->v.b.raw = leaf->v.b.eff = !strcmp(e, "true");
    }
    else {
      leaf->vkind    = UF_CONFIG_KIND_STRING;
      const char *s  = e;
      size_t      sl = strlen(s);
      if (sl >= 2 && s[0] == '"' && s[sl - 1] == '"') {
        s++;
        sl -= 2;
      }
      leaf->v.s.raw     = ConfigArenaStrndup(&h->arena, s, sl);
      leaf->v.s.raw_len = leaf->v.s.eff_len = sl;
      leaf->v.s.eff     = leaf->v.s.raw;
    }
  }
  UfConfigStatus ast = sMarkArrays(root);
  if (ast) {
    ConfigArenaFree(h->arena);
    h->arena = old_a;
    pthread_mutex_unlock(&h->build_lock);
    return ast;
  }
  UfConfigLoadMode mode = opt ? opt->mode : UF_CONFIG_LOAD_LENIENT;
  UfConfigStatus   st   = ConfigApplySchema(h, root, mode, report);
  if (st && st != UF_CONFIG_ERR_REQUIRED && st != UF_CONFIG_ERR_UNKNOWN_FIELD) {
    ConfigArenaFree(h->arena);
    h->arena = old_a;
    pthread_mutex_unlock(&h->build_lock);
    return st;
  }
  /* Into a local, not the handle: the digest is part of what a reader looks
     at, and it is published with the rest below. */
  ConfigCanonAndDigest(root, new_d);

  /* The publish.  This is the whole of what a reader can collide with -- a few
     stores and the index rebuild -- against a parse, a schema application and a
     canonicalisation that no longer exclude anyone. */
  pthread_rwlock_wrlock(&h->lock);
  if (atomic_load(&h->readers) == 0) {
    ConfigArenaFree(old_a);
    ConfigArenaFree(h->retired);
    h->retired = NULL;
  }
  else {
    if (old_a) {
      old_a->next = h->retired;
      h->retired  = old_a;
    }
  }
  h->root = root;
  memcpy(h->digest, new_d, 32);
  /* The tree was just replaced, so both indexes point into an arena that is
     gone unless it was retired.  The text path has always rebuilt them; this
     one had not, which made every reload through the pairs path leave them
     dangling. */
  ConfigRebuildIndexes(h);
  pthread_rwlock_unlock(&h->lock);
  pthread_mutex_unlock(&h->build_lock);
  return UF_CONFIG_OK;
}

PUBLIC_API void UfConfigFreePairs(UfConfigFieldPair *p, size_t n)
{
  if (!p) return;
  for (size_t i = 0; i < n; i++) {
    free((char*)p[i].path);
    free((char*)p[i].encoded);
  }
  free(p);
}

PUBLIC_API UfConfigStatus UfConfigRefreshField(UfConfig *h, const char *path, UfConfigValue *out)
{
  if (!h || !path || !out) return UF_CONFIG_ERR_INVALID_ARG;
  return UfConfigGetField(h, path, out);
}
