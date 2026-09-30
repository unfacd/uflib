/**
 * @file cdt_lockless_lru_priv.h
 * @brief Lockless approximate LRU cache — private implementation details.
 *
 * This header defines the internal structs (LruSlot, LocklessLru) and
 * declares all helper functions used by cdt_lockless_lru.c.
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

#ifndef UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_PRIV_H
#define UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_PRIV_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/cdt/lockless_lru/cdt_lockless_lru.h>
#include <uflib/cdt/lockless_lru/cdt_lockless_lru_type.h>
#include <uflib/cdt/lockless_lru/cdt_lockless_lru_defs.h>
#include <uflib/logger/logger_type.h>

/* ── Slot sentinel values ───────────────────────────────────────────────── */

enum {
    CDT_LOCKLESS_LRU_SLOT_EMPTY     = 0,
    CDT_LOCKLESS_LRU_SLOT_OCCUPIED  = 1,
    CDT_LOCKLESS_LRU_SLOT_TOMBSTONE = 2
};

/* ── Internal types ─────────────────────────────────────────────────────── */

/*!
 * A single slot in the open-addressing hash table.
 *
 * The @p seq field is a seqlock version counter: even = stable (reader may
 * proceed), odd = write-locked (reader must retry).  Writers increment seq
 * (odd) on entry and increment again (even) on exit.
 *
 * @p gen is a generation counter bumped on every occupy / replace / evict /
 * remove.  It lets LocklessLruRefStillValid() detect slot reuse between a
 * GetRef() snapshot and a later check.
 *
 * Cache-line alignment (64 bytes via @p _pad) prevents false sharing between
 * adjacent slots under concurrent access.  Verified by _Static_assert.
 */
typedef struct {
    _Atomic(uint32_t)          seq;         ///< Seqlock version (even=stable, odd=locked)
    _Atomic(uint8_t)           state;       ///< CDT_LOCKLESS_LRU_SLOT_EMPTY / OCCUPIED / TOMBSTONE
    _Atomic(bool)              referenced;  ///< CLOCK second-chance bit
    _Atomic(uint64_t)          key;         ///< Stored key (uint64_t hash)
    _Atomic(LruClientData *)   data;        ///< Caller-owned payload
    _Atomic(uint64_t)          gen;         ///< Generation — bumped on reuse
    char                        _pad[32];   ///< Cache-line pad to 64 bytes
} LruSlot;

_Static_assert(sizeof(LruSlot) == 64, "LruSlot must be cache-line sized (64 bytes)");

/*!
 * Lockless approximate LRU cache instance.
 *
 * All fields are private — consumers receive an opaque handle
 * (forward-declared typedef in cdt_lockless_lru_type.h).
 *
 * Cache-line layout:
 *   Line 0 (bytes  0– 63): slots, capacity, max_items, clock_hand, uf_logger
 *   Line 1 (bytes 64–127): count (isolated — no false sharing with clock_hand)
 *
 * @p clock_hand and @p count are both hot atomics updated on every
 * eviction and every insert/remove respectively.  Without isolation they
 * would share a cache line, causing false-sharing invalidation between
 * a thread doing eviction (write clock_hand) and a thread doing insert
 * (write count).  The @p _pad member pushes @p count to its own cache
 * line, eliminating this conflict.
 *
 * @p uf_logger is borrowed, never owned, and never touched by Get/Set/Remove:
 * it is read only when the handle is created or released.  It is declared after
 * @p clock_hand and its 8 bytes are taken out of @p _pad rather than added after
 * it, which keeps every existing field — including @p count's line-1 isolation —
 * at exactly the offset it had before.  Appending it instead would push
 * @p count to offset 72 and the assertion below would refuse to compile.
 */
struct LocklessLru {
    LruSlot          *slots;       ///< Read-mostly (set at init)
    size_t            capacity;    ///< Read-mostly (set at init)
    size_t            max_items;   ///< Read-mostly (set at init)
    _Atomic(size_t)   clock_hand;  ///< HOT — CLOCK sweep cursor (every eviction)
    UfLogger         *uf_logger;   ///< Borrowed diagnostic sink (write-once); NULL = silent.
    char              _pad[24];    ///< Cache-line isolation: push count to line 1
    _Atomic(size_t)   count;       ///< HOT — current item count (every insert/remove)
};

_Static_assert(offsetof(struct LocklessLru, clock_hand) < 64,
               "clock_hand must be in cache line 0");
_Static_assert(offsetof(struct LocklessLru, count) >= 64,
               "count must be in cache line 1 (isolated from clock_hand)");

/* ── Internal helpers (declared for test white-box access) ──────────────── */

size_t
sNextPow2(size_t n);

uint64_t
sMix64(uint64_t x);

uint32_t
sSlotLock(LruSlot *slot_ptr);

bool
sSlotTryLock(LruSlot *slot_ptr, uint32_t *out_seq_ptr);

void
sSlotUnlock(LruSlot *slot_ptr, uint32_t base_seq);

void
sSlotPeek(LruSlot *slot_ptr, uint8_t *state_out_ptr, uint64_t *key_out_ptr);

int
sSlotRead(LruSlot *slot_ptr, uint8_t *state_out_ptr, uint64_t *key_out_ptr,
          LruClientData **data_out_ptr, uint64_t *gen_out_ptr);

void
sCountSub(LocklessLru *lru_ptr);

void
sTryCollapseTombstone(LocklessLru *lru_ptr, size_t idx);

LruClientData *
sClockEvict(LocklessLru *lru_ptr);

#endif /* UFLIB_CDT_LOCKLESS_LRU_CDT_LOCKLESS_LRU_PRIV_H */
