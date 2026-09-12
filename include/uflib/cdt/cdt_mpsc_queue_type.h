/**
 * @file cdt_mpsc_queue_type.h
 * @brief Type definitions for the intrusive lock-free multi-producer,
 *        single-consumer (MPSC) queue.
 *
 * @warning ABI-frozen layout.  `LocklessMpscQueue` is embedded by value in
 * downstream consumer structs (ufnetcorelib session/delegator types) and
 * `sizeof(struct mpsc_queue_node)` sizes downstream recycler type pools.
 * Field order, field types, and struct sizes must not change outside a
 * MAJOR release.  As of uflib 2.0.0 the queue ends are cache-line
 * isolated (sizeof 192 on LP64): padding-only, deliberately without
 * `alignas`, so malloc'd embedders remain valid; measured ~2x throughput
 * over the packed 48-byte layout.  See
 * technical_designs/UFLIB_CDS_INTRUSIVE_MPSC_QUEUE_DESIGN.md.
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

#ifndef UFLIB_CDT_CDT_MPSC_QUEUE_TYPE_H
#define UFLIB_CDT_CDT_MPSC_QUEUE_TYPE_H

#include <uflib/main_types.h>
#include <uflib/standard_c_includes.h>

/** Cache-line size for padding to prevent false sharing. */
#ifndef UFLIB_CDT_CACHE_LINE_SIZE
#define UFLIB_CDT_CACHE_LINE_SIZE 64
#endif

/**
 * @brief Opaque per-node client payload pointer.
 *
 * Callers cast their payload pointers to/from this type.
 */
typedef void QueueContextData;

/** Convenience cast: wrap any pointer as queue payload. */
#define AS_QUEUE_CONTEXT_DATA(x) ((QueueContextData *)(x))

/**
 * @brief Intrusive queue node.
 *
 * Nodes are allocated and owned by the caller (typically from a recycler
 * type pool).  A node belongs to exactly one queue at a time; ownership
 * passes to the queue on mpsc_queue_insert() and returns to the consumer
 * on mpsc_queue_pop().
 */
typedef struct mpsc_queue_node {
  _Atomic(struct mpsc_queue_node *) next; ///< Successor link; written by the owning producer, read with acquire by the consumer.
  QueueContextData *context_data;         ///< User payload per node.
  struct {
    void (*callback)(ClientContextData *); ///< Optional finaliser invoked by the consumer's node-disposal policy.
    ClientContextData *context_data;       ///< Argument passed to the finaliser callback.
  } finaliser;
} mpsc_queue_node;

/**
 * @brief Lock-free intrusive MPSC queue (Vyukov algorithm).
 *
 * - `head` is the producer end: every mpsc_queue_insert() atomically
 *   exchanges it (wait-free).
 * - `tail` is the consumer end: only the single consumer thread reads or
 *   writes it.
 * - `stub` is an embedded dummy node that decouples the two ends so the
 *   queue never becomes fully empty from the producers' perspective.
 *
 * The three regions are padded onto separate cache lines: `head` is
 * RFO'd by every producer XCHG while `tail`/`stub` are the consumer's
 * hot path — co-residency would bounce one line between all cores on
 * every operation (measured ~2x throughput cost).  Padding-only (no
 * `alignas`): members >= one line apart land on distinct lines at any
 * base alignment, and the struct keeps alignof 8 so existing malloc'd
 * embedders remain valid.
 *
 * @b Thread-safety: Any number of producer threads; exactly one consumer
 * thread.  See cdt_mpsc_queue.h for the per-function contract.
 */
typedef struct LocklessMpscQueue {
  /* ── Producer-hot line: XCHG target for all producers ─────────────── */
  _Atomic(struct mpsc_queue_node *) head; ///< Producer end (XCHG target for all producers).
  unsigned char _pad_head_[UFLIB_CDT_CACHE_LINE_SIZE - sizeof(void *)];

  /* ── Consumer-private line ─────────────────────────────────────────── */
  _Atomic(struct mpsc_queue_node *) tail; ///< Consumer end (consumer-private).
  unsigned char _pad_tail_[UFLIB_CDT_CACHE_LINE_SIZE - sizeof(void *)];

  /* ── Transition line: stub (producer-written on drain re-prime) ───── */
  struct mpsc_queue_node stub;            ///< Embedded dummy node (payload fields unused).
  unsigned char _pad_stub_[UFLIB_CDT_CACHE_LINE_SIZE - sizeof(struct mpsc_queue_node)];
} LocklessMpscQueue;

/**
 * @brief Result of a single non-blocking consumer poll attempt.
 */
enum mpsc_queue_poll_result {
  MPSC_QUEUE_EMPTY, ///< No item available.
  MPSC_QUEUE_ITEM,  ///< An item was dequeued.
  MPSC_QUEUE_RETRY, ///< A producer is mid-insert; retry (item not yet linked).
};

#endif /* UFLIB_CDT_CDT_MPSC_QUEUE_TYPE_H */
