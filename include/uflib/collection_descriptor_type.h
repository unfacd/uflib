/**
 * @file collection_descriptor_type.h
 * @brief collection_descriptor_type
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 12/09/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef UFLIB_COLLECTION_DESCRIPTOR_TYPE_H
#define UFLIB_COLLECTION_DESCRIPTOR_TYPE_H
#include <stddef.h>

//simple mechanism to describe dynamic arrays of objects
typedef void collection_t;

typedef struct CollectionDescriptor CollectionDescriptor;

struct CollectionDescriptor
{
  collection_t **collection;
  size_t         collection_sz;
  size_t         collection_base_offset;
  ///< size offset to use where collection elements are referenced by value. keep '0' for pointer refs.
                                       ///<Allows collection to be chunked up on arbitrary boundaries, not just base  pointer size

  void (*on_destroy_collection)(void *);
};

#endif //UFLIB_COLLECTION_DESCRIPTOR_TYPE_H
