/**
 * @file cdt_lockless_minheap_priv.h
 * @brief Lock-free integer min-priority queue — private implementation details.
 *
 * Defines the internal node (Harris-list link + intrusive Treiber retire
 * node) and the opaque queue struct.  The layout invariants that the reclaim
 * path depends on are locked by _Static_assert below.
 *
 * THIS FILE IS NOT INSTALLED.  It is private to the module implementation
 * and its tests.  Consumers must never include this file.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_LOCKLESS_MINHEAP_CDT_LOCKLESS_MINHEAP_PRIV_H
#define UFLIB_CDT_LOCKLESS_MINHEAP_CDT_LOCKLESS_MINHEAP_PRIV_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/cdt/lockless_minheap/cdt_lockless_minheap.h>
#include <uflib/cdt/lockless_minheap/cdt_lockless_minheap_type.h>
#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack_type.h>
#include <uflib/logger/logger_type.h>

/* ── Internal types ─────────────────────────────────────────────────────── */

/*!
 * A node in the Harris lock-free ordered list.
 *
 * @p retire is an intrusive LocklessTreiberStackNode embedded as the FIRST
 * member, so a retired node can be pushed onto the reclaim stack without a
 * second allocation and recovered with `container()` (offset 0).
 *
 * @p next is the Harris-list link; its least-significant bit is the mark
 * (tombstone): bit 0 clear = live, bit 0 set = logically deleted.
 *
 * @p key and @p value are immutable after publication — they are written once
 * before the node is linked and never mutated, so readers that reach a node
 * through an acquire edge can read them without further synchronisation.
 */
struct LocklessMinHeapNode {
    struct LocklessTreiberStackNode        retire;  ///< Intrusive retire link (first member).
    _Atomic(struct LocklessMinHeapNode *) next;     ///< Harris link; LSB = marked.
    int64_t                                key;     ///< Inline key (immutable after publish).
    void                                  *value;   ///< Caller-owned payload (immutable).
};

_Static_assert(offsetof(struct LocklessMinHeapNode, retire) == 0,
               "retire must be the first member (container() relies on it)");

/*!
 * Opaque lock-free min-PQ instance.
 *
 * @p head is a dummy sentinel (key INT64_MIN) that is never retired; it is
 * written once at create and read thereafter.  @p retired is the intrusive
 * retire stack, also written once at create.  Only @p approx_size is written
 * on the hot path, so it is isolated onto its own cache line (the pad is
 * defensive — @p head/@p retired have no co-writer, but the isolation keeps
 * the counter from colliding with any future hot field).
 *
 * @p uf_logger is borrowed, never owned, and never touched by insert/delmin:
 * it is read only when the handle is created or released, which is why it sits
 * in the cold group rather than behind a pointer of its own.  NULL means the
 * queue reports nothing, which is the behaviour every caller had before the
 * field existed.
 */
struct LocklessMinHeap {
    struct LocklessMinHeapNode *head;        ///< Dummy sentinel (write-once).
    LocklessTreiberStack       *retired;     ///< Retire stack (write-once).
    UfLogger                   *uf_logger;   ///< Borrowed diagnostic sink (write-once); NULL = silent.
    char                        _pad[40];    ///< Isolate approx_size to its own line.
    _Atomic(int64_t)            approx_size; ///< HOT — relaxed telemetry (signed; may transiently go negative, clamped on read).
};

_Static_assert(offsetof(struct LocklessMinHeap, approx_size) >= 64,
               "approx_size must be isolated onto its own cache line");

#endif /* UFLIB_CDT_LOCKLESS_MINHEAP_CDT_LOCKLESS_MINHEAP_PRIV_H */
