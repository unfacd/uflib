/**
 * @file buffer_descriptor.h
 * @brief Routines for manipulating BufferDescriptor structure
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

#ifndef UFLIB_BUFFER_DESCRIPTOR_H
#define UFLIB_BUFFER_DESCRIPTOR_H

#include <uflib/uflib_defs.h>
#include <uflib/buffer_descriptor/buffer_descriptor_type.h>

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/**
 * @brief Initialise a BufferDescriptor and allocate its backing buffer.
 *
 * Allocates @p initial_cap bytes for the content buffer, clamped to a minimum
 * of 64 bytes and a maximum of 1 MiB.  On allocation failure the descriptor is
 * left empty (bd->data == NULL, size == 0, size_max == 0); because the function
 * returns void, callers must check bd->data after the call to detect failure.
 *
 * @param bd           Pointer to the descriptor to initialise.  NULL is a no-op.
 * @param initial_cap  Requested initial capacity in bytes (clamped to [64, 1 MiB]).
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 256);
 * if (bd.data == NULL) {
 *     fprintf(stderr, "out of memory\n");
 *     return;
 * }
 * @endcode
 */
PUBLIC_API void BufferDescriptorInit(BufferDescriptor *bd, size_t initial_cap);

/**
 * @brief Free a BufferDescriptor's backing buffer and reset it to empty.
 *
 * Idempotent: safe to call more than once, or on a zero-initialised descriptor
 * (a second call is a harmless free(NULL)).
 *
 * @param bd  Descriptor to release.  NULL is a no-op.
 */
PUBLIC_API void BufferDescriptorRelease(BufferDescriptor *bd);

/* ── Core operations ───────────────────────────────────────────────────── */

/**
 * @brief Append printf-style formatted output to the buffer.
 *
 * Formats @p fmt together with its trailing variadic arguments and appends the
 * result to the buffer's existing content, growing the backing allocation as
 * needed (doubling up to the 1 MiB cap).  The buffer is always NUL-terminated
 * on success.
 *
 * @param bd   Initialised descriptor to append to.
 * @param fmt  printf-style format string, followed by its variadic arguments.
 * @return BUFFER_DESCRIPTOR_OK on success; otherwise a specific
 *         BUFFER_DESCRIPTOR_ERR_* code — BUFFER_DESCRIPTOR_ERR_NULL_ARG for a
 *         NULL @p bd or @p fmt, BUFFER_DESCRIPTOR_ERR_ENCODING, or
 *         BUFFER_DESCRIPTOR_ERR_OVERFLOW / BUFFER_DESCRIPTOR_ERR_CAP /
 *         BUFFER_DESCRIPTOR_ERR_ALLOC from the underlying grow step.
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 64);
 * if (BufferDescriptorAppendFormatted(&bd, "%s:%d", "port", 8080) != BUFFER_DESCRIPTOR_OK) {
 *     fprintf(stderr, "append failed\n");
 *     BufferDescriptorRelease(&bd);
 *     return;
 * }
 * printf("%s\n", bd.data);   // "port:8080"
 * BufferDescriptorRelease(&bd);
 * @endcode
 */
PUBLIC_API BufferDescriptorResult BufferDescriptorAppendFormatted(BufferDescriptor *bd, const char *fmt, ...);

#endif //UFLIB_BUFFER_DESCRIPTOR_H
