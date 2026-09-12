/**
 * @file buffer_descriptor_type.h
 * @brief buffer_descriptor_type
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 30/08/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef UFLIB_BUFFER_DESCRIPTOR_TYPE_H
#define UFLIB_BUFFER_DESCRIPTOR_TYPE_H

#include <stddef.h>

/**
 * @brief Return codes for BufferDescriptor operations.
 *
 * Success is 0; error codes are negative, so a caller may test failure with a
 * simple `!= BUFFER_DESCRIPTOR_OK` check.
 */
typedef enum BufferDescriptorResult
{
  BUFFER_DESCRIPTOR_OK             =  0,  ///< Operation succeeded.
  BUFFER_DESCRIPTOR_ERR_NULL_ARG   = -1,  ///< NULL descriptor or format argument.
  BUFFER_DESCRIPTOR_ERR_ENCODING   = -2,  ///< vsnprintf formatting/encoding error.
  BUFFER_DESCRIPTOR_ERR_OVERFLOW   = -3,  ///< Size arithmetic overflow.
  BUFFER_DESCRIPTOR_ERR_CAP        = -4,  ///< Capacity cap (1 MiB) exceeded.
  BUFFER_DESCRIPTOR_ERR_ALLOC      = -5,  ///< Memory allocation failed.
} BufferDescriptorResult;

typedef struct BufferDescriptor
{
  char   *data;      ///< Allocated backing buffer, NUL-terminated at data[size].
  size_t  size;      ///< Current content length in bytes (excluding the NUL terminator).
  size_t  size_max;  ///< Allocated capacity of data in bytes.
} BufferDescriptor;


#endif //UFLIB_BUFFER_DESCRIPTOR_TYPE_H
