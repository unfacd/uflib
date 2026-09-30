/**
 * @file ufconfig_namespace.c
 * @brief Namespace walking: the parsed tree presented as navigable named tables.
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
#include <stdlib.h>
#include <string.h>

struct UfConfigNamespace
{
  const UfConfig *h;
  UfNode *        node;     /* defining node; alias is not pre-followed */
  UfNode *        resolved; /* target if alias, else node */
  const char *    path;
  const char *    via_path;
};

/*!
 * @brief Whether this namespace carries a @c #bindings holder.
 *
 * Only such a namespace can hold one name under two kinds.  A document ending
 * in an explicit return block binds each name as an alias, while the top-level
 * binding that declared it is spliced in under @c #bindings -- so the same
 * table is reachable twice, once as each.  Where there is no holder there is
 * nothing to reconcile, and the reconciliation below stays off so that an
 * ordinary wide scope does not pay for it.
 */
static int sHasBindings(const UfConfigNamespace *ns)
{
  for (const UfNode *c = ns->resolved->first_child; c; c = c->next_sibling)
    if (c->name && strcmp(c->name, "#bindings") == 0) return 1;
  return 0;
}

/*!
 * @brief Whether an entry with the same name precedes @p target in entry order.
 *
 * The two are one table seen twice.  Reporting both makes a walk show the name
 * twice with no way to tell they are the same, and makes the positional and
 * named lookups disagree about which one they mean -- which is exactly what
 * this module's namespace test was written to hold visible.  The first wins,
 * and the first is the alias, which resolves to the other.
 */
static int sNameSeenEarlier(const UfConfigNamespace *ns, const UfNode *target)
{
  for (const UfNode *c = ns->resolved->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) {
      for (const UfNode *b = c->first_child; b; b = b->next_sibling) {
        if (b == target) return 0;
        if (b->name && target->name && strcmp(b->name, target->name) == 0) return 1;
      }
      continue;
    }
    if (c == target) return 0;
    if (c->name && target->name && strcmp(c->name, target->name) == 0) return 1;
  }
  return 0;
}

/*! @brief Fills @p out from one entry node. */
static UfConfigStatus sFillEntry(const UfConfigNamespace *ns, UfNode *item, UfConfigNamespaceEntry *out)
{
  memset(out, 0, sizeof(*out));
  out->name = item->name;
  if (item->kind == UF_NODE_ALIAS) {
    out->kind         = UF_CONFIG_NS_TABLE_ALIAS;
    out->alias_target = item->alias_target;
    UfConfigNamespaceGet(ns->h, item->path ? item->path : item->alias_target, &out->table);
  }
  else if (item->kind == UF_NODE_ARRAY) {
    out->kind = UF_CONFIG_NS_ARRAY;
    ConfigNodeToValue(item, &out->value);
  }
  else if (item->kind == UF_NODE_SCOPE) {
    out->kind = UF_CONFIG_NS_TABLE_INLINE;
    UfConfigNamespaceGet(ns->h, item->path, &out->table);
  }
  else {
    out->kind = UF_CONFIG_NS_VALUE;
    ConfigNodeToValue(item, &out->value);
  }
  return UF_CONFIG_OK;
}


PUBLIC_API UfConfigStatus UfConfigNamespaceGet(const UfConfig *h, const char *path, UfConfigNamespace **out)
{
  if (!h || !out) return UF_CONFIG_ERR_INVALID_ARG;
  /* The resolve reads h->root and both indexes, and a reload swaps them under
     the write lock.  Held only for the resolve: the handle that comes back
     carries raw node pointers that the caller uses in later calls, and no lock
     taken here can keep those alive.  That is what a pin is for, and what the
     header's note about a namespace not outliving a reload is about. */
  pthread_rwlock_rdlock((pthread_rwlock_t*)&h->lock);
  UfNode *n = (path && path[0]) ? ConfigResolvePath(h, path) : h->root;
  if (!n) {
    pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
    return UF_CONFIG_ERR_NOFIELD;
  }
  if (n->kind == UF_NODE_LEAF) {
    pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
    return UF_CONFIG_ERR_NOT_A_TABLE;
  }
  UfConfigNamespace *ns = (UfConfigNamespace*)calloc(1, sizeof(*ns));
  if (!ns) {
    pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
    return UF_CONFIG_ERR_NO_MEMORY;
  }
  ns->h    = h;
  ns->node = n;
  if (n->kind == UF_NODE_ALIAS && n->resolved) {
    ns->resolved = n->resolved;
    ns->path     = n->path ? n->path : path;
  }
  else {
    ns->resolved = n;
    ns->path     = n->path ? n->path : path;
  }
  *out = ns;
  pthread_rwlock_unlock((pthread_rwlock_t*)&h->lock);
  return UF_CONFIG_OK;
}

PUBLIC_API UfConfigStatus UfConfigRootGet(const UfConfig *h, UfConfigNamespace **out)
{
  return UfConfigNamespaceGet(h, "", out);
}

PUBLIC_API size_t UfConfigNamespaceEntryCount(const UfConfigNamespace *ns)
{
  if (!ns || !ns->resolved) return 0;
  int    dedup = sHasBindings(ns);
  size_t k     = 0;
  for (UfNode *c = ns->resolved->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) {
      for (UfNode *b = c->first_child; b; b = b->next_sibling) {
        if (dedup && sNameSeenEarlier(ns, b)) continue;
        k++;
      }
    }
    else if (!(dedup && sNameSeenEarlier(ns, c))) k++;
  }
  return k;
}

PUBLIC_API UfConfigStatus UfConfigNamespaceEntryAt(const UfConfigNamespace *ns, size_t idx, UfConfigNamespaceEntry *out)
{
  if (!ns || !out) return UF_CONFIG_ERR_INVALID_ARG;
  int    dedup = sHasBindings(ns);
  size_t k     = 0;
  for (UfNode *c = ns->resolved->first_child; c; c = c->next_sibling) {
    if (c->name && strcmp(c->name, "#bindings") == 0) {
      for (UfNode *b = c->first_child; b; b = b->next_sibling) {
        if (dedup && sNameSeenEarlier(ns, b)) continue;
        if (k++ != idx) continue;
        return sFillEntry(ns, b, out);
      }
      continue;
    }
    if (dedup && sNameSeenEarlier(ns, c)) continue;
    if (k++ != idx) continue;
    return sFillEntry(ns, c, out);
  }
  return UF_CONFIG_ERR_NOFIELD;
}

PUBLIC_API UfConfigStatus UfConfigNamespaceEntryByName(const UfConfigNamespace *ns, const char *name,
                                                       UfConfigNamespaceEntry * out)
{
  if (!ns || !name) return UF_CONFIG_ERR_INVALID_ARG;
  size_t n = UfConfigNamespaceEntryCount(ns);
  for (size_t i = 0; i < n; i++) {
    UfConfigNamespaceEntry e;
    memset(&e, 0, sizeof(e));
    if (UfConfigNamespaceEntryAt(ns, i, &e)) continue;
    if (e.name && strcmp(e.name, name) == 0) {
      *out = e;
      return UF_CONFIG_OK;
    }
    if (e.table) free(e.table);
  }
  return UF_CONFIG_ERR_NOFIELD;
}

PUBLIC_API UfConfigStatus UfConfigNamespaceResolve(const UfConfigNamespace *ns, UfConfigNamespace **out)
{
  if (!ns || !out) return UF_CONFIG_ERR_INVALID_ARG;
  if (ns->node && ns->node->kind == UF_NODE_ALIAS && ns->node->resolved) return UfConfigNamespaceGet(
    ns->h, ns->node->resolved->path, out);
  return UfConfigNamespaceGet(ns->h, ns->path, out);
}

PUBLIC_API const char *UfConfigNamespacePath(const UfConfigNamespace *ns)
{
  return ns ? ns->path : NULL;
}

PUBLIC_API const char *UfConfigNamespacePathVia(const UfConfigNamespace *ns)
{
  return ns ? (ns->via_path ? ns->via_path : ns->path) : NULL;
}

PUBLIC_API UfConfigStatus UfConfigNamespaceToJson(const UfConfigNamespace *ns, char *buf, size_t cap, size_t *needed)
{
  if (!ns) return UF_CONFIG_ERR_INVALID_ARG;
  return ConfigEmitJson(ns->resolved ? ns->resolved : ns->node, buf, cap, needed, false, true);
}

PUBLIC_API UfConfigStatus UfConfigNamespaceToYaml(const UfConfigNamespace *ns, char *buf, size_t cap, size_t *needed)
{
  if (!ns) return UF_CONFIG_ERR_INVALID_ARG;
  return ConfigEmitYaml(ns->node, buf, cap, needed, true);
}

PUBLIC_API UfConfigStatus UfConfigNamespaceToIni(const UfConfigNamespace *ns, char *buf, size_t cap, size_t *needed)
{
  if (!ns) return UF_CONFIG_ERR_INVALID_ARG;
  return ConfigEmitIni(ns->node, buf, cap, needed);
}
