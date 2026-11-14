/**
 * @file resource_router.c
 * @brief ResourceRouter implementation — segment trie over registered routes.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/resource_router/resource_router.h>

#include "resource_router_type_priv.h"

#define RESOURCE_ROUTE_METHODS_ALL \
  (RESOURCE_ROUTE_METHOD_GET | RESOURCE_ROUTE_METHOD_DEL | RESOURCE_ROUTE_METHOD_PUT)

static bool sPathIsValid(const char *path);
static bool sPathSame(const char *a, const char *b);
static int sSegmentCmp(const char *segment, size_t len, const char *child_segment);
static const ResourceRouteNode *sFindChild(const ResourceRouteNode *node, const char *segment, size_t len);
static ResourceRouteNode *sFindOrCreateChild(ResourceRouteNode *node, const char *segment, size_t len);
static ResourceRouteNode *sFindNode(ResourceRouteRegistry *registry, const char *path);
static void sDestroyNode(ResourceRouteNode *node);
static const char *sMethodName(unsigned method);
static bool sEmitJsonString(BufferDescriptor *buffer, const char *text);
static bool sEmitLeaf(const ResourceRouteNode *node, BufferDescriptor *buffer);
static bool sEmitChildren(const ResourceRouteNode *node, BufferDescriptor *buffer);
static bool sEmitNode(const ResourceRouteNode *node, BufferDescriptor *buffer);

static bool
sPathIsValid(const char *path)
{
  const char *p;

  if (!path) {
    return false;
  }

  p = path;
  while (*p == '/') {
    p++;
  }
  if (!*p) {
    return false;
  }

  for (;;) {
    const char *end = p;

    while (*end && *end != '/') {
      end++;
    }
    if (end == p) {
      return false;
    }
    if (!*end) {
      return true;
    }

    p = end + 1;
    if (!*p || *p == '/') {
      return false;
    }
  }
}

static bool
sPathSame(const char *a, const char *b)
{
  while (*a == '/') {
    a++;
  }
  while (*b == '/') {
    b++;
  }
  return strcmp(a, b) == 0;
}

static int
sSegmentCmp(const char *segment, size_t len, const char *child_segment)
{
  size_t child_len = strlen(child_segment);
  size_t min_len   = len < child_len ? len : child_len;
  int    cmp       = memcmp(segment, child_segment, min_len);

  if (cmp) {
    return cmp;
  }
  if (len < child_len) {
    return -1;
  }
  if (len > child_len) {
    return 1;
  }
  return 0;
}

static const ResourceRouteNode *
sFindChild(const ResourceRouteNode *node, const char *segment, size_t len)
{
  size_t lo = 0;
  size_t hi = node->n_children;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int    cmp = sSegmentCmp(segment, len, node->children[mid]->segment);

    if (cmp == 0) {
      return node->children[mid];
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }

  return NULL;
}

static ResourceRouteNode *
sFindOrCreateChild(ResourceRouteNode *node, const char *segment, size_t len)
{
  const ResourceRouteNode *found;
  ResourceRouteNode       *child;
  size_t                   pos;

  found = sFindChild(node, segment, len);
  if (found) {
    return (ResourceRouteNode *)found;
  }

  child = calloc(1, sizeof(*child));
  if (!child) {
    return NULL;
  }
  child->segment = strndup(segment, len);
  if (!child->segment) {
    free(child);
    return NULL;
  }

  if (node->n_children == node->cap_children) {
    size_t               new_cap = node->cap_children ? node->cap_children * 2 : 4;
    ResourceRouteNode  **grown   = realloc(node->children, new_cap * sizeof(*grown));

    if (!grown) {
      free(child->segment);
      free(child);
      return NULL;
    }
    node->children      = grown;
    node->cap_children  = new_cap;
  }

  for (pos = 0; pos < node->n_children; pos++) {
    if (sSegmentCmp(segment, len, node->children[pos]->segment) < 0) {
      break;
    }
  }
  memmove(&node->children[pos + 1], &node->children[pos],
          (node->n_children - pos) * sizeof(*node->children));
  node->children[pos] = child;
  node->n_children++;

  return child;
}

static ResourceRouteNode *
sFindNode(ResourceRouteRegistry *registry, const char *path)
{
  ResourceRouteNode *node = &registry->root;
  const char        *p    = path;

  while (*p == '/') {
    p++;
  }

  while (*p) {
    const char *end = p;

    while (*end && *end != '/') {
      end++;
    }
    node = (ResourceRouteNode *)sFindChild(node, p, (size_t)(end - p));
    if (!node) {
      return NULL;
    }
    p = end;
    while (*p == '/') {
      p++;
    }
  }

  return node;
}

static void
sDestroyNode(ResourceRouteNode *node)
{
  for (size_t i = 0; i < node->n_children; i++) {
    sDestroyNode(node->children[i]);
  }
  free(node->children);
  free(node->segment);
  free(node->description);
  free(node);
}

static const char *
sMethodName(unsigned method)
{
  switch (method) {
  case RESOURCE_ROUTE_METHOD_GET: return "GET";
  case RESOURCE_ROUTE_METHOD_DEL: return "DEL";
  case RESOURCE_ROUTE_METHOD_PUT: return "PUT";
  default:                        return "?";
  }
}

static bool
sEmitJsonString(BufferDescriptor *buffer, const char *text)
{
  const char *p;

  if (BufferDescriptorAppendFormatted(buffer, "\"") != BUFFER_DESCRIPTOR_OK) {
    return false;
  }

  for (p = text; *p; p++) {
    unsigned char c = (unsigned char)*p;

    switch (c) {
    case '"':
    case '\\':
      if (BufferDescriptorAppendFormatted(buffer, "\\%c", c) != BUFFER_DESCRIPTOR_OK) {
        return false;
      }
      break;
    case '\n':
      if (BufferDescriptorAppendFormatted(buffer, "\\n") != BUFFER_DESCRIPTOR_OK) {
        return false;
      }
      break;
    case '\r':
      if (BufferDescriptorAppendFormatted(buffer, "\\r") != BUFFER_DESCRIPTOR_OK) {
        return false;
      }
      break;
    case '\t':
      if (BufferDescriptorAppendFormatted(buffer, "\\t") != BUFFER_DESCRIPTOR_OK) {
        return false;
      }
      break;
    default:
      if (c < 0x20) {
        if (BufferDescriptorAppendFormatted(buffer, "\\u%04x", c) != BUFFER_DESCRIPTOR_OK) {
          return false;
        }
      } else if (BufferDescriptorAppendFormatted(buffer, "%c", c) != BUFFER_DESCRIPTOR_OK) {
        return false;
      }
      break;
    }
  }

  return BufferDescriptorAppendFormatted(buffer, "\"") == BUFFER_DESCRIPTOR_OK;
}

static bool
sEmitChildren(const ResourceRouteNode *node, BufferDescriptor *buffer)
{
  bool   first = true;
  size_t i;

  if (BufferDescriptorAppendFormatted(buffer, "{") != BUFFER_DESCRIPTOR_OK) {
    return false;
  }

  for (i = 0; i < node->n_children; i++) {
    const ResourceRouteNode *child = node->children[i];

    if (!first && BufferDescriptorAppendFormatted(buffer, ",") != BUFFER_DESCRIPTOR_OK) {
      return false;
    }
    first = false;

    if (!sEmitJsonString(buffer, child->segment ? child->segment : "")
        || BufferDescriptorAppendFormatted(buffer, ":") != BUFFER_DESCRIPTOR_OK
        || !sEmitNode(child, buffer)) {
      return false;
    }
  }

  return BufferDescriptorAppendFormatted(buffer, "}") == BUFFER_DESCRIPTOR_OK;
}

static bool
sEmitLeaf(const ResourceRouteNode *node, BufferDescriptor *buffer)
{
  bool     first = true;
  unsigned method;

  if (BufferDescriptorAppendFormatted(buffer, "{\"name\":") != BUFFER_DESCRIPTOR_OK
      || !sEmitJsonString(buffer, node->segment ? node->segment : "")
      || BufferDescriptorAppendFormatted(buffer, ",\"description\":") != BUFFER_DESCRIPTOR_OK
      || !sEmitJsonString(buffer, node->description ? node->description : "")
      || BufferDescriptorAppendFormatted(buffer, ",\"methods\":[") != BUFFER_DESCRIPTOR_OK) {
    return false;
  }

  for (method = RESOURCE_ROUTE_METHOD_GET; method <= RESOURCE_ROUTE_METHOD_PUT; method <<= 1) {
    if (!(node->handler.methods & method)) {
      continue;
    }
    if (!first && BufferDescriptorAppendFormatted(buffer, ",") != BUFFER_DESCRIPTOR_OK) {
      return false;
    }
    first = false;
    if (!sEmitJsonString(buffer, sMethodName(method))) {
      return false;
    }
  }

  return BufferDescriptorAppendFormatted(buffer, "]}") == BUFFER_DESCRIPTOR_OK;
}

static bool
sEmitNode(const ResourceRouteNode *node, BufferDescriptor *buffer)
{
  if (node->n_children > 0) {
    return sEmitChildren(node, buffer);
  }
  return sEmitLeaf(node, buffer);
}

PUBLIC_API ResourceRouteRegistry *
ResourceRouteRegistryCreate(void)
{
  return calloc(1, sizeof(ResourceRouteRegistry));
}

PUBLIC_API void
ResourceRouteRegistryDestroy(ResourceRouteRegistry *registry)
{
  size_t i;

  if (!registry) {
    return;
  }

  for (i = 0; i < registry->root.n_children; i++) {
    sDestroyNode(registry->root.children[i]);
  }
  free(registry->root.children);
  free(registry);
}

PUBLIC_API int
ResourceRouteRegister(ResourceRouteRegistry *registry, const char *path, const char *description,
                      const ResourceRouteHandler *handler, unsigned flags)
{
  ResourceRouteNode *node;
  char              *description_copy = NULL;
  const char        *p;

  if (!registry || !handler) {
    return -1;
  }
  if (!sPathIsValid(path)) {
    return -1;
  }
  if (!(handler->methods & RESOURCE_ROUTE_METHODS_ALL) || (handler->methods & ~RESOURCE_ROUTE_METHODS_ALL)) {
    return -1;
  }
  if (flags & ~(unsigned)RESOURCE_ROUTE_STEMMED) {
    return -1;
  }

  node = sFindNode(registry, path);
  if (node && node->handler.methods) {
    return -1;
  }

  if (description) {
    description_copy = strdup(description);
    if (!description_copy) {
      return -1;
    }
  }

  p    = path;
  node = &registry->root;
  while (*p == '/') {
    p++;
  }
  while (*p) {
    const char *end = p;

    while (*end && *end != '/') {
      end++;
    }
    node = sFindOrCreateChild(node, p, (size_t)(end - p));
    if (!node) {
      free(description_copy);
      return -1;
    }
    p = end;
    while (*p == '/') {
      p++;
    }
  }

  node->handler = *handler;
  node->flags   = flags;
  free(node->description);
  node->description = description_copy;

  return 0;
}

PUBLIC_API int
ResourceRouteRegisterTable(ResourceRouteRegistry *registry, const ResourceRouteEntry *table, size_t n)
{
  size_t i;

  if (!registry || (!table && n)) {
    return 1;
  }

  for (i = 0; i < n; i++) {
    const ResourceRouteEntry *entry = &table[i];
    ResourceRouteNode        *existing;

    if (!sPathIsValid(entry->path) || !entry->handler) {
      return (int)(i + 1);
    }
    if (!(entry->handler->methods & RESOURCE_ROUTE_METHODS_ALL)
        || (entry->handler->methods & ~RESOURCE_ROUTE_METHODS_ALL)) {
      return (int)(i + 1);
    }
    if (entry->flags & ~(unsigned)RESOURCE_ROUTE_STEMMED) {
      return (int)(i + 1);
    }

    existing = sFindNode(registry, entry->path);
    if (existing && existing->handler.methods) {
      return (int)(i + 1);
    }

    for (size_t j = 0; j < i; j++) {
      if (sPathSame(table[j].path, entry->path)) {
        return (int)(i + 1);
      }
    }
  }

  for (i = 0; i < n; i++) {
    if (ResourceRouteRegister(registry, table[i].path, table[i].description,
                              table[i].handler, table[i].flags) != 0) {
      return (int)(i + 1);
    }
  }

  return 0;
}

PUBLIC_API ResourceRouteMatchResult
ResourceRouteMatch(ResourceRouteRegistry *registry, unsigned method, const char *path,
                   const ResourceRouteHandler **handler, const char **remainder)
{
  const ResourceRouteNode *node;
  const char              *p;

  if (handler) {
    *handler = NULL;
  }
  if (remainder) {
    *remainder = NULL;
  }

  if (!registry || !path) {
    return RESOURCE_ROUTE_NO_MATCH;
  }
  if (!method || (method & (method - 1)) || (method & ~RESOURCE_ROUTE_METHODS_ALL)) {
    return RESOURCE_ROUTE_NO_MATCH;
  }

  p = path;
  while (*p == '/') {
    p++;
  }
  if (!*p) {
    return RESOURCE_ROUTE_NO_MATCH;
  }

  node = &registry->root;
  for (;;) {
    const char              *end = p;
    const ResourceRouteNode *child;

    while (*end && *end != '/') {
      end++;
    }
    child = sFindChild(node, p, (size_t)(end - p));
    if (!child) {
      return RESOURCE_ROUTE_NO_MATCH;
    }
    node = child;

    p = end;
    while (*p == '/') {
      p++;
    }

    if (node->flags & RESOURCE_ROUTE_STEMMED) {
      if (!(node->handler.methods & method)) {
        return RESOURCE_ROUTE_METHOD_NOT_ALLOWED;
      }
      if (handler) {
        *handler = &node->handler;
      }
      if (remainder) {
        *remainder = p;
      }
      return RESOURCE_ROUTE_MATCH;
    }

    if (!*p) {
      break;
    }
  }

  if (!(node->handler.methods & method)) {
    return node->handler.methods ? RESOURCE_ROUTE_METHOD_NOT_ALLOWED : RESOURCE_ROUTE_NO_MATCH;
  }

  if (handler) {
    *handler = &node->handler;
  }

  return RESOURCE_ROUTE_MATCH;
}

PUBLIC_API BufferDescriptor *
ResourceRouteRegistryDescribe(ResourceRouteRegistry *registry, BufferDescriptor *buffer)
{
  if (!registry || !buffer) {
    return NULL;
  }

  if (!sEmitChildren(&registry->root, buffer)) {
    return NULL;
  }

  return buffer;
}
