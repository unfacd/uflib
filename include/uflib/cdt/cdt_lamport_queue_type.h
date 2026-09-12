/**
 * @file cdt_lamport_queue_type.h
 * @brief Type definitions for the Lamport SPSC lock-free FIFO queue.
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

#ifndef UFLIB_CDT_CDT_LAMPORT_QUEUE_TYPE_H
#define UFLIB_CDT_CDT_LAMPORT_QUEUE_TYPE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

/** Cache-line size for padding to prevent false sharing. */
#define UFLIB_CDT_CACHE_LINE_SIZE 64

/**
 * @brief Opaque client-data pointer stored in queue slots.
 *
 * Callers cast their payload pointers to/from this type.
 */
typedef void QueueClientData;

/** Convenience cast: wrap any pointer as queue payload. */
#define AS_QUEUE_CLIENT_DATA(x) ((QueueClientData *)(x))

/**
 * @brief Lock-free single-producer, single-consumer (SPSC) FIFO queue.
 *
 * Based on Leslie Lamport's SPSC circular-buffer algorithm.  One slot is
 * reserved to distinguish full from empty, so a queue initialised with
 * @p queue_sz slots has a usable capacity of (@p queue_sz - 1).
 *
 * Fields are arranged to minimise cache-line contention:
 * - The hot atomic path (`front_`, `back_`, `leased`, `queue_sz`) sits on
 *   the first cache line, read-mostly by both threads.
 * - `cached_front_` is producer-private and sits on its own cache line.
 * - `cached_back_` is consumer-private and sits on its own cache line.
 * - `payload` and `owns_payload` are cold (touched only at Init/Destroy).
 *
 * @b Thread-safety: Exactly one producer and one consumer thread.
 */
typedef struct LamportQueue {
    /* ── Hot path: read-mostly by both threads ──────────────────── */
    atomic_size_t front_;         /**< Consumer writes, producer reads (cached). */
    atomic_size_t back_;          /**< Producer writes, consumer reads (cached). */
    atomic_size_t leased;         /**< Approximate leased-element count. */
    size_t        queue_sz;       /**< Total slot count (usable: queue_sz - 1).  */

    /* ── Producer-private cache line ────────────────────────────── */
    size_t        cached_front_;  /**< Local snapshot of front_ for producer. */
    unsigned char _pad_prod_[UFLIB_CDT_CACHE_LINE_SIZE - sizeof(size_t)];

    /* ── Consumer-private cache line ────────────────────────────── */
    size_t        cached_back_;   /**< Local snapshot of back_ for consumer. */
    unsigned char _pad_cons_[UFLIB_CDT_CACHE_LINE_SIZE - sizeof(size_t)];

    /* ── Cold / init-only ───────────────────────────────────────── */
    QueueClientData **payload;    /**< Array of queue_sz pointers (the ring buffer). */
    bool              owns_payload; /**< True if payload was calloc'd by Init. */
} LamportQueue;

/** Alternate name — both refer to the same struct. */
typedef struct LamportQueue LocklessSpscQueue;

#endif /* UFLIB_CDT_CDT_LAMPORT_QUEUE_TYPE_H */
#if 0
/**
 * @file cdt_lamport_queue_type.h
 * @brief Type definitions for the Lamport SPSC lock-free FIFO queue.
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

#ifndef UFLIB_CDT_CDT_LAMPORT_QUEUE_TYPE_H
#define UFLIB_CDT_CDT_LAMPORT_QUEUE_TYPE_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

/*
 * Cache-line size used to separate producer-private and consumer-private
 * fields and thereby avoid false sharing.
 *
 * Override before including this header, e.g.:
 *
 *   #define UFLIB_CDT_CACHE_LINE_SIZE 128
 *   #include <uflib/cdt/cdt_lamport_queue_type.h>
 *
 * Default is 64, which is correct for virtually all contemporary x86-64,
 * aarch64 and RISC-V cores.  Larger values (e.g. 128) are harmless and may
 * be preferable on platforms that advertise a 128-byte L1 line or when the
 * structure is placed in a region subject to aggressive prefetch.
 */
#ifndef UFLIB_CDT_CACHE_LINE_SIZE
#define UFLIB_CDT_CACHE_LINE_SIZE 64
#endif

/**
 * @brief Opaque client-data pointer stored in queue slots.
 *
 * Callers cast their payload pointers to/from this type.
 */
typedef void QueueClientData;

/** Convenience cast: wrap any pointer as queue payload. */
#define AS_QUEUE_CLIENT_DATA(x) ((QueueClientData *)(x))

/**
 * @brief Lock-free single-producer, single-consumer (SPSC) FIFO queue.
 *
 * Based on Leslie Lamport's SPSC circular-buffer algorithm.  One slot is
 * reserved to distinguish full from empty, so a queue initialised with
 * @p queue_sz slots has a usable capacity of (@p queue_sz - 1).
 *
 * Fields are arranged to minimise cache-line contention:
 * - The hot atomic path (`front_`, `back_`, `leased`, `queue_sz`) occupies
 *   the first cache line (or the beginning of the structure).
 * - `cached_front_` is producer-private and forced onto its own cache line
 *   via `alignas`.
 * - `cached_back_` is consumer-private and likewise isolated.
 * - `payload` and `owns_payload` are cold (touched only at Init/Destroy)
 *   and also start on a fresh cache line so that rare updates never bounce
 *   the consumer's private line.
 *
 * Manual char-array padding has been replaced by C11 `alignas`.  This is
 * more robust across architectures, compilers and future changes to member
 * sizes: the compiler guarantees the required alignment instead of relying
 * on arithmetic that can become incorrect if `sizeof(size_t)` or packing
 * attributes change.
 *
 * @b Thread-safety: Exactly one producer and one consumer thread.
 */
//If you later want the whole object to be cache-line aligned when allocated on the stack or via malloc, you can also write
//typedef struct alignas(UFLIB_CDT_CACHE_LINE_SIZE) LamportQueue {
//	...
//} LamportQueue;

typedef struct LamportQueue {
	/* ── Hot path: read-mostly by both threads ──────────────────── */
	atomic_size_t front_;         /**< Consumer writes, producer reads (cached). */
	atomic_size_t back_;          /**< Producer writes, consumer reads (cached). */
	atomic_size_t leased;         /**< Approximate leased-element count. */
	size_t        queue_sz;       /**< Total slot count (usable: queue_sz - 1).  */

	/* ── Producer-private cache line ────────────────────────────── */
	alignas(UFLIB_CDT_CACHE_LINE_SIZE)
	size_t        cached_front_;  /**< Local snapshot of front_ for producer. */

	/* ── Consumer-private cache line ────────────────────────────── */
	alignas(UFLIB_CDT_CACHE_LINE_SIZE)
	size_t        cached_back_;   /**< Local snapshot of back_ for consumer. */

	/* ── Cold / init-only ───────────────────────────────────────── */
	alignas(UFLIB_CDT_CACHE_LINE_SIZE)
	QueueClientData **payload;    /**< Array of queue_sz pointers (the ring buffer). */
	bool              owns_payload; /**< True if payload was calloc'd by Init. */
} LamportQueue;

/** Alternate name — both refer to the same struct. */
typedef struct LamportQueue LocklessSpscQueue;

/*
 * Compile-time sanity checks.
 * The first private field must be at least one cache line beyond the start
 * of the structure (or beyond the hot fields).  These asserts catch an
 * accidental reduction of UFLIB_CDT_CACHE_LINE_SIZE below a useful value.
 */
_Static_assert(UFLIB_CDT_CACHE_LINE_SIZE >= 32,
               "UFLIB_CDT_CACHE_LINE_SIZE must be at least 32");
_Static_assert((UFLIB_CDT_CACHE_LINE_SIZE & (UFLIB_CDT_CACHE_LINE_SIZE - 1)) == 0,
               "UFLIB_CDT_CACHE_LINE_SIZE must be a power of two");

#endif /* UFLIB_CDT_CDT_LAMPORT_QUEUE_TYPE_H */
#endif
