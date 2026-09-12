/**
 * @file utils_buffer_descriptor.c
 * @brief utils_buffer_descriptor
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>

#ifndef BUFFER_DESCRIPTOR_MAX_BYTES
#  define BUFFER_DESCRIPTOR_MAX_BYTES  (1024 * 1024)
#endif

PUBLIC_API void
BufferDescriptorInit(BufferDescriptor *bd, size_t initial_cap)
{
  if (!bd) return;

  if (initial_cap < 64) initial_cap = 64;
  if (initial_cap > BUFFER_DESCRIPTOR_MAX_BYTES) initial_cap = BUFFER_DESCRIPTOR_MAX_BYTES;

  bd->data     = malloc(initial_cap);
  bd->size     = 0;
  bd->size_max = bd->data ? initial_cap : 0;
  if (bd->data) bd->data[0] = '\0';
}

PUBLIC_API void
BufferDescriptorRelease(BufferDescriptor *bd)
{
  if (!bd) return;
  free(bd->data);

  bd->data     = NULL;
  bd->size     = 0;
  bd->size_max = 0;
}

/**
 * @brief Ensure the buffer can hold @p need additional bytes plus a NUL terminator.
 *
 * Grows the backing allocation if necessary, doubling the current capacity
 * (starting at 64 bytes when unallocated) and clamping straight to the 1 MiB
 * cap once doubling would exceed it.  On allocation failure the descriptor is
 * left unchanged.
 *
 * @param bd    Descriptor to grow.  Must be non-NULL (the caller checks).
 * @param need  Additional bytes to make room for (excludes the NUL terminator).
 * @return BUFFER_DESCRIPTOR_OK on success (including the already-sufficient
 *         case); BUFFER_DESCRIPTOR_ERR_OVERFLOW, BUFFER_DESCRIPTOR_ERR_CAP, or
 *         BUFFER_DESCRIPTOR_ERR_ALLOC otherwise.
 */
static BufferDescriptorResult
sBufferDescriptorReserve(BufferDescriptor *bd, size_t need)
{
  size_t want = bd->size + need + 1;

  if (want < bd->size) return BUFFER_DESCRIPTOR_ERR_OVERFLOW;
  if (want > BUFFER_DESCRIPTOR_MAX_BYTES) return BUFFER_DESCRIPTOR_ERR_CAP;
  if (bd->data && want <= bd->size_max) return BUFFER_DESCRIPTOR_OK;

  size_t ncap = bd->size_max ? bd->size_max : 64;
  if (!bd->data) ncap = 64;

  while (ncap < want) {
    if (ncap > BUFFER_DESCRIPTOR_MAX_BYTES / 2) {
      ncap = BUFFER_DESCRIPTOR_MAX_BYTES;
      break;
    }
    ncap *= 2;
  }

  if (ncap < want || ncap > BUFFER_DESCRIPTOR_MAX_BYTES) return BUFFER_DESCRIPTOR_ERR_CAP;

  char *p = realloc(bd->data, ncap);
  if (!p) return BUFFER_DESCRIPTOR_ERR_ALLOC;
  bd->data     = p;
  bd->size_max = ncap;
  if (bd->size == 0) bd->data[0] = '\0';

  return BUFFER_DESCRIPTOR_OK;
}

PUBLIC_API BufferDescriptorResult
BufferDescriptorAppendFormatted(BufferDescriptor *bd, const char *fmt, ...)
{
  if (!bd || !fmt)
    return BUFFER_DESCRIPTOR_ERR_NULL_ARG;

  va_list ap;
  va_start(ap, fmt);
  int needed = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  if (needed < 0) return BUFFER_DESCRIPTOR_ERR_ENCODING;

  BufferDescriptorResult rc = sBufferDescriptorReserve(bd, (size_t)needed);
  if (rc != BUFFER_DESCRIPTOR_OK) return rc;

  va_start(ap, fmt);
  vsnprintf(bd->data + bd->size, (size_t)needed + 1, fmt, ap);
  va_end(ap);
  bd->size += (size_t)needed;

  return BUFFER_DESCRIPTOR_OK;
}