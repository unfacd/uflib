/**
 * @file cdt_lockless_ringbuffer_priv.h
 * @brief Lock-free bounded ring buffer — private implementation details.
 *
 * Defines the internal struct LocklessRingBuffer and the per-slot sequence
 * protocol invariants.  THIS FILE IS NOT INSTALLED — it is private to the
 * module implementation and its tests.  Consumers must never include it.
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

#ifndef UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_PRIV_H
#define UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_PRIV_H

#include <uflib/standard_c_includes.h>

#include <uflib/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer.h>
#include <uflib/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer_type.h>
#include <uflib/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer_defs.h>
#include <uflib/logger/logger_type.h>

#define UFLIB_CDT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE \
        PRIV_CONFIG_DEFAULT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE

/*!
 * Lock-free bounded ring buffer instance.
 *
 * All fields are private — consumers receive an opaque handle
 * (forward-declared typedef in cdt_lockless_ringbuffer_type.h).
 *
 * Cache-line layout (LP64: sizeof(size_t) == 8, sizeof(pointer) == 8,
 * sizeof(LocklessRingBufferMode) == 4):
 *
 *   Line 0 (bytes  0– 63): cold, read-mostly metadata (capacity, usable,
 *                          mask, elem_size, seq, data, uf_logger, mode).
 *   Line 1 (bytes 64–127): `tail` — producer cursor, RFO'd by every producer.
 *   Line 2 (bytes 128–191): `head` — consumer cursor, RFO'd by every consumer.
 *
 * `tail` and `head` are the two hot atomics: without isolation a producer
 * writing `tail` and a consumer writing `head` would ping-pong the same
 * cache line on every operation (measured ~2x throughput cost in the
 * sibling LocklessMpscQueue).  The `_pad_*` members push each cursor onto
 * its own line; the offsets are locked by _Static_assert below.
 *
 * @p uf_logger is borrowed, never owned, and never touched by push/pop: it is
 * read only when the handle is created or released, which is why it belongs to
 * the cold group and why its 8 bytes come out of @p _pad_meta rather than being
 * added after it — appending it would move @p tail off line 1 and the offset
 * assertion below would (correctly) refuse to compile.
 *
 * @p uf_logger is declared ahead of @p mode, not after it: a 4-byte enum
 * followed by an 8-byte pointer leaves 4 bytes of implicit alignment padding
 * that @p _pad_meta cannot see, and the cold group would no longer be exactly
 * one cache line.  Declaring the pointer first keeps the group at 60 bytes with
 * a 4-byte pad and no implicit holes.  This moved @p mode from offset 48 to 56;
 * the struct is private, so nothing outside this module observes the offset.
 */
struct LocklessRingBuffer {
    /* Cold metadata — read-mostly after create (cache line 0). */
    size_t              capacity;      ///< Storage slot count (power of two).
    size_t              usable;        ///< Storable elements (capacity − 1 for SPSC, else capacity).
    size_t              mask;          ///< capacity − 1 (index mask).
    size_t              elem_size;     ///< Bytes per element.
    _Atomic(size_t)    *seq;           ///< Per-slot sequence numbers (MPSC/MPMC only; NULL for SPSC).
    unsigned char      *data;          ///< Raw element storage.
    UfLogger           *uf_logger;     ///< Borrowed diagnostic sink (write-once); NULL = silent.
    LocklessRingBufferMode mode;       ///< Concurrency mode (SPSC/MPSC/MPMC).
    char                _pad_meta[UFLIB_CDT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE
                                   - (2 * sizeof(size_t) /* capacity, usable */
                                   + 2 * sizeof(size_t) /* mask, elem_size */
                                   + sizeof(_Atomic(size_t) *) /* seq */
                                   + sizeof(unsigned char *) /* data */
                                   + sizeof(UfLogger *) /* uf_logger */
                                   + sizeof(LocklessRingBufferMode))]; /* mode */

    /* Producer cursor (cache line 1). */
    _Atomic(size_t)     tail;          ///< Producer cursor: number of successful pushes.
    char                _pad_tail[UFLIB_CDT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE
                                   - sizeof(_Atomic(size_t))];

    /* Consumer cursor (cache line 2). */
    _Atomic(size_t)     head;          ///< Consumer cursor: number of successful pops.
    char                _pad_head[UFLIB_CDT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE
                                   - sizeof(_Atomic(size_t))];
};

_Static_assert(offsetof(struct LocklessRingBuffer, tail) == UFLIB_CDT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE,
               "tail must begin on cache line 1 (offset 64)");
_Static_assert(offsetof(struct LocklessRingBuffer, head) == 2 * UFLIB_CDT_LOCKLESS_RINGBUFFER_CACHE_LINE_SIZE,
               "head must begin on cache line 2 (offset 128)");

#endif /* UFLIB_CDT_LOCKLESS_RINGBUFFER_CDT_LOCKLESS_RINGBUFFER_PRIV_H */
