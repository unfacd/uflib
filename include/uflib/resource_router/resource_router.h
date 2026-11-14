/**
 * @file resource_router.h
 * @brief ResourceRouter — public API for the path-keyed route registry.
 *
 * Registries are heap-allocated and opaque. Every operation takes the handle
 * returned by @c ResourceRouteRegistryCreate(), and any number of registries may
 * coexist in one process.
 *
 * The registry holds no lock: the caller completes registration before any
 * thread matches, and serialises the registry's lifecycle.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_RESOURCE_ROUTER_RESOURCE_ROUTER_H
#define UFLIB_RESOURCE_ROUTER_RESOURCE_ROUTER_H

#include <stddef.h>

#include <uflib/uflib_defs.h>
#include <uflib/resource_router/resource_router_type.h>

/**
 * @brief Allocate an empty registry.
 *
 * @return The new registry handle, or @c NULL on allocation failure.
 */
PUBLIC_API ResourceRouteRegistry *ResourceRouteRegistryCreate(void);

/**
 * @brief Release every registered route and the registry itself.
 *
 * @param[in] registry Handle from @c ResourceRouteRegistryCreate(); @c NULL is a no-op.
 */
PUBLIC_API void ResourceRouteRegistryDestroy(ResourceRouteRegistry *registry);

/**
 * @brief Register one route.
 *
 * @param[in] registry    Handle from @c ResourceRouteRegistryCreate().
 * @param[in] path        '/'-separated route path. Must not be empty.
 * @param[in] description Human-readable text, copied; may be @c NULL.
 * @param[in] handler     Descriptor, copied by value.
 * @param[in] flags       ResourceRouteFlags for this path.
 *
 * @return 0 on success, -1 on a malformed path, duplicate path or allocation failure.
 */
PUBLIC_API int ResourceRouteRegister(ResourceRouteRegistry *registry, const char *path, const char *description,
                                     const ResourceRouteHandler *handler, unsigned flags);

/**
 * @brief Register every row of @p table.
 *
 * Every row is validated before any is registered, so a malformed or duplicate
 * row leaves the registry untouched. An allocation failure while registering
 * leaves the rows before the failing one in place.
 *
 * @param[in] registry Handle from @c ResourceRouteRegistryCreate().
 * @param[in] table    Static table of routes.
 * @param[in] n        Number of rows in @p table.
 *
 * @return 0 on success, otherwise the 1-based index of the row that failed.
 */
PUBLIC_API int ResourceRouteRegisterTable(ResourceRouteRegistry *registry, const ResourceRouteEntry *table, size_t n);

/**
 * @brief Match @p method and @p path against the registry.
 *
 * @param[in]  registry  Handle from @c ResourceRouteRegistryCreate().
 * @param[in]  method    One ResourceRouteMethod bit.
 * @param[in]  path      '/'-separated path, without a leading route prefix.
 * @param[out] handler   Receives the matched descriptor. May be @c NULL.
 * @param[out] remainder Receives the tail past a stemmed route, @c NULL for an
 *                       exact non-stemmed match. May be @c NULL.
 *
 * @return RESOURCE_ROUTE_MATCH and the descriptor in @p handler when the route
 *         advertises @p method; RESOURCE_ROUTE_METHOD_NOT_ALLOWED when the route
 *         exists but advertises another method; RESOURCE_ROUTE_NO_MATCH otherwise.
 */
PUBLIC_API ResourceRouteMatchResult
ResourceRouteMatch(ResourceRouteRegistry *registry, unsigned method, const char *path,
                   const ResourceRouteHandler **handler, const char **remainder);

/**
 * @brief Emit a JSON description of the registry.
 *
 * One object per nesting level, keyed by segment. A registered route emits
 * @code
 * {"name":"suspend","description":"...","methods":["GET","PUT"],"stemmed":false}
 * @endcode
 * and appends @c "children":{...} when it also has descendants. A node that is
 * only a path group emits an object of its children. @c methods is empty when
 * none are advertised; @c description is @c null when none was registered.
 *
 * @param[in]     registry Handle from @c ResourceRouteRegistryCreate().
 * @param[in,out] buffer   Initialised destination buffer; must not be @c NULL.
 *
 * @return @p buffer, or @c NULL on allocation failure or a @c NULL argument.
 */
PUBLIC_API BufferDescriptor *ResourceRouteRegistryDescribe(ResourceRouteRegistry *registry, BufferDescriptor *buffer);

#endif /* UFLIB_RESOURCE_ROUTER_RESOURCE_ROUTER_H */
