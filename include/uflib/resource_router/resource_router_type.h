/**
 * @file resource_router_type.h
 * @brief Public type definitions for ResourceRouter — opaque-handle, path-keyed
 *        route registry with per-route callbacks.
 *
 * A registry maps '/'-separated path segments to handler descriptors. Each
 * registration carries its own flags; a route registered with
 * @c RESOURCE_ROUTE_STEMMED short-circuits matching at that node and passes the
 * remainder of the path to the handler.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_RESOURCE_ROUTER_RESOURCE_ROUTER_TYPE_H
#define UFLIB_RESOURCE_ROUTER_RESOURCE_ROUTER_TYPE_H

#include <stdbool.h>
#include <stddef.h>

#include <uflib/buffer_descriptor/buffer_descriptor_type.h>

/*! Route method bits. @c ResourceRouteMatch() takes one of these. */
typedef enum ResourceRouteMethod
{
  RESOURCE_ROUTE_METHOD_GET = 1u << 0, ///< GET
  RESOURCE_ROUTE_METHOD_DEL = 1u << 1, ///< DEL
  RESOURCE_ROUTE_METHOD_PUT = 1u << 2  ///< PUT
} ResourceRouteMethod;

/*! Per-registration flags. Flags describe the path, not the handler. */
typedef enum ResourceRouteFlags
{
  RESOURCE_ROUTE_NONE    = 0,         ///< Exact-match route
  RESOURCE_ROUTE_STEMMED = 1u << 0    ///< Stop matching here; pass the remainder to the handler
} ResourceRouteFlags;

/*! Outcome of a match attempt. Distinguishes an unknown path from a known path
 *  that does not advertise the requested method. */
typedef enum ResourceRouteMatchResult
{
  RESOURCE_ROUTE_NO_MATCH          = 0, ///< No route for the path
  RESOURCE_ROUTE_MATCH             = 1, ///< Route matched and advertises the method
  RESOURCE_ROUTE_METHOD_NOT_ALLOWED = 2 ///< Route exists; a different method is advertised
} ResourceRouteMatchResult;

/*! Context handed to a route callback.
 *
 * @c remainder points into the caller's path string and is valid only for the
 * duration of the call: it is @c "" when a stemmed route was addressed directly,
 * the text after the stem's last segment when there is a tail, and @c NULL when
 * the matched route was not stemmed. Copy it if it must outlive the callback.
 */
typedef struct ResourceHandlerContext
{
  const char       *remainder; ///< Tail past the matched route; @c NULL for an exact, non-stemmed match
  void             *user_data; ///< The registration-time pointer from @c ResourceRouteHandler
  BufferDescriptor *buffer;    ///< Caller-supplied reply buffer; may be @c NULL
} ResourceHandlerContext;

/*! Route callback. Returning @c NULL means "no reply", not an error. */
typedef BufferDescriptor *(*ResourceRouteCallback)(ResourceHandlerContext *ctx);

/*! Handler descriptor registered for a path.
 *
 * Copied by value at registration; the caller's struct may be a stack temporary.
 * @c methods gates dispatch before the callback is invoked.
 */
typedef struct ResourceRouteHandler
{
  ResourceRouteCallback on_get;    ///< GET callback
  ResourceRouteCallback on_del;    ///< DEL callback
  ResourceRouteCallback on_put;    ///< PUT callback
  unsigned int          methods;   ///< ResourceRouteMethod bitmask
  void                 *user_data; ///< Handed to the callback as @c ctx->user_data
} ResourceRouteHandler;

/*! One row of a bulk registration table. */
typedef struct ResourceRouteEntry
{
  const char                 *path;        ///< '/'-separated route path
  const char                 *description; ///< Copied at registration; may be NULL
  const ResourceRouteHandler *handler;     ///< Descriptor copied at registration
  unsigned                    flags;       ///< ResourceRouteFlags
} ResourceRouteEntry;

/*! Route registry handle — opaque. */
typedef struct ResourceRouteRegistry ResourceRouteRegistry;

#endif /* UFLIB_RESOURCE_ROUTER_RESOURCE_ROUTER_TYPE_H */
