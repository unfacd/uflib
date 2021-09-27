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

#ifndef SRC_INCLUDE_MAIN_TYPES_H_
#define SRC_INCLUDE_MAIN_TYPES_H_

#include <stddef.h>

typedef void  ClientContextData;
typedef	void 	CommandContextData;
typedef void MessageContextData;
typedef void ItemContainer;

#include "standard_defs.h"
#include "utils_str.h"

#define CLIENT_CTX_DATA(x)	((ClientContextData *)(x))
#define AS_CLIENT_CONTEXT_DATA(x)	((ClientContextData *)(x))
#define COMMAND_CTX_DATA(x)	((CommandContextData *)(x))

#define CALLFLAGS_EMPTY 0

#define RETURN_BUFFER_UNALLOCATED NULL

#define _LOCK_TRY_FLAG_FALSE	0
#define _LOCK_TRY_FLAG_TRUE		1

#define FLAG_SELF_DESTRUCT_TRUE   true
#define FLAG_SELF_DESTRUCT_FALSE  false

#define AS_COLLECTION_TYPE(x)  ((collection_t	**)(x))
#define AS_CONST_CHAR_TYPE(x) ((const char *)(x))

typedef struct ContextDataPair {
	ClientContextData *first;
	ClientContextData	*second;
} ContextDataPair;

//simple mechanism to describe dynamic arrays of objects
typedef void collection_t;
typedef struct CollectionDescriptor {
	collection_t	**collection;
	size_t			collection_sz;
	size_t      collection_base_offset; //size offset to use where collection elements are referenced by value. keep '0' for pointer refs. Allows collection to be chunked up on arbitrary boundaries, not just base  pointer size

	//void (*free_collection)(void *);
}	CollectionDescriptor;

typedef struct CollectionDescriptorPair {
	CollectionDescriptor 	first,
												second;
}	CollectionDescriptorPair;

typedef struct BufferDescriptor {
	char 		*data;
	size_t	size,
					size_max;
} BufferDescriptor;

#define INIT_FLAG_TRUE    true
#define INIT_FLAG_FALSE   false

#endif /* SRC_INCLUDE_MAIN_TYPES_H_ */
