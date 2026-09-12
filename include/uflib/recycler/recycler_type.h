/**
 * Copyright (C) 2015-2021 unfacd works
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
 * @file recycler_type.h
 * @brief Recycler public types — RecyclerPoolHandle, RecyclerPoolOps
 */

#ifndef UFLIB_RECYCLER_RECYCLER_TYPE_H
#define UFLIB_RECYCLER_RECYCLER_TYPE_H

#include "instance_type.h"

typedef void RecyclerClientData;
typedef void ClientData;
typedef void ContextData;

typedef int (*type_instantiator)(ContextData **, unsigned long, int);

#define RECYCLER_CLIENT_DATA(x)   (RecyclerClientData)(x)
#define AS_CONTEXT_DATA(x)        ((ContextData *)(x))
#define NO_CONTEXT_DATA           NULL

typedef struct RecyclerPoolHandle {
		unsigned type;
		const char *type_name;
		size_t blocksz;
}	RecyclerPoolHandle;

typedef struct RecyclerPoolOps {
	int (*poolop_init_callback)(ClientContextData *, size_t);//when storage allocation has occurred. No InstanceHolder ref available yet
	int (*poolop_initget_callback)(InstanceHolder *, ContextData *, size_t, unsigned long);//initialiser for when fetched from the pool
	int (*poolop_initput_callback)(InstanceHolder *, ContextData *, unsigned long);//initialiser for when put back into the pool
	char *(*poolop_print_callback)(InstanceHolder *, ContextData *, unsigned long);//print yourself
	int (*poolop_destruct_callback)(InstanceHolder *, ContextData *, unsigned long);
	int (*poolop_instantiator_callback)(ContextData **, unsigned long, int);//user-defined instantiator of main object type

} RecyclerPoolOps;

#endif /* UFLIB_RECYCLER_RECYCLER_TYPE_H */
