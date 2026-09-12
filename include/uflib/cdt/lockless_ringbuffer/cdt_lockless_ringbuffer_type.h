/**
 * @file cdt_lockless_ringbuffer_type.h
 * @brief Lock-free bounded ring buffer — type definitions (opaque handle and
 *        concurrency-mode enum).
 *
 * This header contains only the opaque handle forward-declaration and the
 * mode enum.  Consumers that need to embed a ring-buffer handle by value can
 * include this file without pulling in the full API surface.
 *
 * Copyright (C) 2015-2026 unfacd works
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

#ifndef UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_TYPE_H
#define UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_TYPE_H

#include <stddef.h>

/*!
 * Opaque handle to a lock-free bounded ring buffer instance.
 *
 * The struct definition is private
 * (src/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer_priv.h).  Consumers
 * never allocate or inspect it directly — they obtain it via
 * LocklessRingBufferCreate() and destroy it via LocklessRingBufferDestroy().
 */
typedef struct LocklessRingBuffer LocklessRingBuffer;

/*!
 * Concurrency mode, selected at creation and fixed for the lifetime of the
 * instance.  The mode dictates the thread-safety contract:
 *
 *   - SPSC — exactly one producer and one consumer thread (fastest path).
 *   - MPSC — any number of producer threads, exactly one consumer thread.
 *   - MPMC — any number of producer and consumer threads.
 *
 * The enumerator values are part of the public ABI and must never be
 * renumbered (locked by static_assert in the gtest suite).
 */
typedef enum {
    LOCKLESS_RINGBUF_MODE_SPSC = 0,   ///< Single producer / single consumer (fastest).
    LOCKLESS_RINGBUF_MODE_MPSC = 1,   ///< Multiple producers / single consumer.
    LOCKLESS_RINGBUF_MODE_MPMC = 2    ///< Multiple producers / multiple consumers.
} LocklessRingBufferMode;

#endif /* UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_TYPE_H */
