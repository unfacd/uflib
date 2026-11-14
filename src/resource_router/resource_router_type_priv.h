/**
 * @file resource_router_type_priv.h
 * @brief Private type definitions for ResourceRouter — NOT INSTALLED.
 *
 * Colocated with resource_router.c. The registry struct is opaque in the public
 * API (<uflib/resource_router/resource_router_type.h>); consumers must not
 * include this file.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_SRC_RESOURCE_ROUTER_TYPE_PRIV_H
#define UFLIB_SRC_RESOURCE_ROUTER_TYPE_PRIV_H

#include <stdbool.h>
#include <stddef.h>

#include <uflib/resource_router/resource_router_type.h>

/*! One path segment. Owned strings; the root's @c segment is NULL. */
typedef struct ResourceRouteNode
{
  char             *segment;      ///< Segment name, owned; NULL on the root
  char             *description;  ///< Registered description, owned; NULL if none
  ResourceRouteHandler handler;   ///< Registered descriptor; zeroed until registered
  unsigned          flags;        ///< ResourceRouteFlags for this path
  struct ResourceRouteNode **children; ///< Sorted by segment
  size_t            n_children;
  size_t            cap_children;
} ResourceRouteNode;

struct ResourceRouteRegistry
{
  ResourceRouteNode root;         ///< Root of the segment trie
};

#endif /* UFLIB_SRC_RESOURCE_ROUTER_TYPE_PRIV_H */
