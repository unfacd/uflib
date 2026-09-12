/**
 * Copyright (C) 2015-2025 unfacd works
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

/**
 * @file recycler.h
 * @brief Recycler slab allocator — public API for type-pooled object lifecycle management
 */

#ifndef UFLIB_RECYCLER_RECYCLER_H
#define UFLIB_RECYCLER_RECYCLER_H

#include <uflib/uflib_defs.h>

#include <stdatomic.h>
#include <uflib/recycler/instance_type.h>
#include <uflib/recycler/recycler_type.h>

#define INSTANCE_HOLDER_FOUND     1
#define INSTANCE_HOLDER_NOT_FOUND 0

// Backward-compatibility shims — to be removed after consumer migration
#define _ONCE_ 1
#define _TWICE_ 2
#define _INCREMENT_REFERENCE(x) (x)
#define _DECREMENT_REFERENCE(x) (x)

PUBLIC_API RecyclerPoolHandle *
RecyclerInitTypePool(const char *type_name, size_t blocksz, size_t group_allocation_sz, size_t expansion_threshold,
                     RecyclerPoolOps *pool_ops_ptr);
PUBLIC_API int RecyclerPut(const unsigned type, InstanceHolder *, ContextData *, unsigned long);
PUBLIC_API InstanceHolder *RecyclerGet(const unsigned type, ContextData *, unsigned long);

PUBLIC_API InstanceHolder *RecyclerGetNewInstance(InstanceHolder *instance_holder_ptr);
PUBLIC_API int RecyclerDestroyInstance(InstanceHolder *instance_holder_ptr);

/**
 * @brief Extract the recycler-managed object pointer from an InstanceHolder.
 *
 * For backward compatibility, this is also available as GetClientContextData().
 * New code should use RecyclerGetClientContextData().
 */
PUBLIC_API ClientContextData *RecyclerGetClientContextData(InstanceHolder *);
#define GetClientContextData RecyclerGetClientContextData

PUBLIC_API InstanceHolder *InstanceHolderFromClientContext(ContextData *ctx_ptr);

PUBLIC_API void RecyclerTypeReferenced(unsigned type, InstanceHolder *client_data, int);
PUBLIC_API void RecyclerTypeUnReferenced(unsigned type, InstanceHolder *client_data, int);
PUBLIC_API size_t RecyclerTypeGetReferenceCount(unsigned type, InstanceHolder *client_data);

#endif /* UFLIB_RECYCLER_RECYCLER_H */
