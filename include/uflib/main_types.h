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

#ifndef UFLIB_MAIN_TYPES_H
#define UFLIB_MAIN_TYPES_H

#include <stddef.h>

typedef void ClientContextData;
typedef	void CommandContextData;
typedef void MessageContextData;
typedef void ItemContainer;

#include <uflib/standard_defs.h>
#include <uflib/utils_str.h>
#include "uflib/collection_descriptor_type.h"
#include <uflib/buffer_descriptor/buffer_descriptor_type.h> //don't remove, as this acts as a shim for consumers who expected this type to be defined in this file

#define CLIENT_CTX_DATA(x)	((ClientContextData *)(x))
#define AS_CLIENT_CONTEXT_DATA(x)	((ClientContextData *)(x))
#define COMMAND_CTX_DATA(x)	((CommandContextData *)(x))

#define CALLFLAGS_EMPTY 0
#define _EMPTY_STR (char *)NULL

#define RETURN_BUFFER_UNALLOCATED NULL

#define _LOCK_TRY_FLAG_FALSE	0
#define _LOCK_TRY_FLAG_TRUE		1

#define FLAG_SELF_DESTRUCT_TRUE   true
#define FLAG_SELF_DESTRUCT_FALSE  false

#define AS_COLLECTION_TYPE(x)  ((collection_t	**)(x))
#define AS_COLLECTION_DESCRIPTOR(x) ((CollectionDescriptor *)(x))
#define AS_COLLECTION_DESCRIPTOR_PAIR(x) ((CollectionDescriptorPair *)(x))
#define AS_CONST_CHAR_TYPE(x) ((const char *)(x))
#define AS_CHAR_TYPE(x) ((char *)(x))

typedef struct ContextDataPair {
	ClientContextData   *first;
	ClientContextData   *second;
} ContextDataPair;

typedef struct CollectionDescriptorPair {
	CollectionDescriptor 	    first,
                            second;
    CollectionDescriptor    source;
} CollectionDescriptorPair;

#define INIT_FLAG_TRUE    true
#define INIT_FLAG_FALSE   false

#endif /* UFLIB_MAIN_TYPES_H */
