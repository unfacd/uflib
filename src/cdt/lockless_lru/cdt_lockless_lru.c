/**
 * @file cdt_lockless_lru.c
 * @brief Lockless approximate LRU cache — CLOCK second-chance implementation
 *        with per-slot seqlocks, open-addressing linear-probe hash table,
 *        bounded eviction, upsert Set, and generation-stamped reads.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include "cdt_lockless_lru_priv.h"

#include <uflib/logger/logger.h>

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ───────────────────────────────────────────────────── */

size_t
sNextPow2(size_t n)
{
    if (n <= 1) return 1;
    if (n > (SIZE_MAX / 2) + 1) return 0;
    size_t p = 1;
    while (p < n) {
        if (p > (SIZE_MAX / 2)) return 0;
        p <<= 1;
    }
    return p;
}

/* SplitMix64 finalizer — decent avalanche for a 64-bit key/hash. */
uint64_t
sMix64(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/*
 * Blocking claim of one slot for writing.  Spins on CAS — the critical
 * section is a handful of atomic stores, so an unbounded spin here is
 * the standard, correct seqlock-writer idiom (not a "real" spinlock:
 * only THIS slot is contended, never the whole table).  A PAUSE is
 * issued every 64 failed attempts to keep the spin cache-friendly.
 */
uint32_t
sSlotLock(LruSlot *slot_ptr)
{
    unsigned spins = 0;
    for (;;) {
        uint32_t seq = atomic_load_explicit(&slot_ptr->seq, memory_order_relaxed);
        if ((seq & 1u) == 0 &&
            atomic_compare_exchange_weak_explicit(&slot_ptr->seq, &seq, seq + 1,
                memory_order_acq_rel, memory_order_relaxed)) {
            return seq;
        }
        if ((++spins & 63u) == 0) {
#if defined(__GNUC__)
            __asm__ __volatile__("pause" ::: "memory");
#endif
        }
    }
}

/*
 * Non-blocking claim, used only by the CLOCK sweep: on contention we
 * want to move the hand to the next slot, not stall.
 */
bool
sSlotTryLock(LruSlot *slot_ptr, uint32_t *out_seq_ptr)
{
    uint32_t seq = atomic_load_explicit(&slot_ptr->seq, memory_order_relaxed);
    if (seq & 1u) return false;
    if (atomic_compare_exchange_strong_explicit(&slot_ptr->seq, &seq, seq + 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        *out_seq_ptr = seq;
        return true;
    }
    return false;
}

void
sSlotUnlock(LruSlot *slot_ptr, uint32_t base_seq)
{
    atomic_store_explicit(&slot_ptr->seq, base_seq + 2, memory_order_release);
}

/*
 * Portable seqlock read of a slot's (state, key) snapshot.  The acquire
 * fence between the payload loads and the second (relaxed) sequence load
 * keeps the payload reads inside the two sequence observations on weak
 * memory models — the C11/C17-portable seqlock idiom.
 */
void
sSlotPeek(LruSlot *slot_ptr, uint8_t *state_out_ptr, uint64_t *key_out_ptr)
{
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&slot_ptr->seq, memory_order_acquire);
        if (s1 & 1u) continue;
        uint8_t  st = atomic_load_explicit(&slot_ptr->state, memory_order_relaxed);
        uint64_t k  = atomic_load_explicit(&slot_ptr->key, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&slot_ptr->seq, memory_order_relaxed);
        if (s1 == s2 && (s1 & 1u) == 0) {
            *state_out_ptr = st;
            *key_out_ptr   = k;
            return;
        }
    }
}

/*
 * Full seqlock read of a slot's (state, key, data, gen) snapshot.  Same
 * portable fence idiom as sSlotPeek, extended to the data and generation
 * fields needed by GetRef / RefStillValid.
 */
int
sSlotRead(LruSlot *slot_ptr, uint8_t *state_out_ptr, uint64_t *key_out_ptr,
          LruClientData **data_out_ptr, uint64_t *gen_out_ptr)
{
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&slot_ptr->seq, memory_order_acquire);
        if (s1 & 1u) continue;
        uint8_t         st = atomic_load_explicit(&slot_ptr->state, memory_order_relaxed);
        uint64_t        k  = atomic_load_explicit(&slot_ptr->key, memory_order_relaxed);
        LruClientData  *d  = atomic_load_explicit(&slot_ptr->data, memory_order_relaxed);
        uint64_t        g  = atomic_load_explicit(&slot_ptr->gen, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&slot_ptr->seq, memory_order_relaxed);
        if (s1 == s2 && (s1 & 1u) == 0) {
            *state_out_ptr = st;
            *key_out_ptr   = k;
            *data_out_ptr  = d;
            *gen_out_ptr   = g;
            return 0;
        }
    }
}

/*
 * Saturating decrement of the item count — never lets count wrap below
 * zero if a future change double-decrements.
 */
void
sCountSub(LocklessLru *lru_ptr)
{
    size_t cur = atomic_load_explicit(&lru_ptr->count, memory_order_relaxed);
    while (cur > 0) {
        if (atomic_compare_exchange_weak_explicit(&lru_ptr->count, &cur, cur - 1,
                memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

/*
 * If slot idx is a tombstone and the next slot is EMPTY, convert idx (and a
 * short backward run of tombstones) to EMPTY so probe chains can terminate.
 *
 * Correctness relies on the open-addressing invariant: an EMPTY successor
 * means no occupied slot's probe path crosses idx (its chain would have
 * stopped at the EMPTY).  The walk is opportunistic and bounded (8 steps,
 * try-lock — gives up on contention).
 */
void
sTryCollapseTombstone(LocklessLru *lru_ptr, size_t idx)
{
    size_t mask = lru_ptr->capacity - 1;
    size_t next = (idx + 1) & mask;
    uint8_t nstate; uint64_t nkey;
    sSlotPeek(&lru_ptr->slots[next], &nstate, &nkey);
    if (nstate != CDT_LOCKLESS_LRU_SLOT_EMPTY) return;

    size_t cur = idx;
    for (size_t steps = 0; steps < 8; steps++) {
        LruSlot *slot_ptr = &lru_ptr->slots[cur];
        uint32_t seq;
        if (!sSlotTryLock(slot_ptr, &seq)) break;
        uint8_t st = atomic_load_explicit(&slot_ptr->state, memory_order_relaxed);
        if (st != CDT_LOCKLESS_LRU_SLOT_TOMBSTONE) {
            sSlotUnlock(slot_ptr, seq);
            break;
        }
        /* Re-check successor still EMPTY (except the first we already peeked). */
        if (steps > 0) {
            uint8_t ns; uint64_t nk;
            sSlotPeek(&lru_ptr->slots[(cur + 1) & mask], &ns, &nk);
            if (ns != CDT_LOCKLESS_LRU_SLOT_EMPTY) {
                sSlotUnlock(slot_ptr, seq);
                break;
            }
        }
        atomic_store_explicit(&slot_ptr->state,
                              (uint8_t)CDT_LOCKLESS_LRU_SLOT_EMPTY,
                              memory_order_relaxed);
        sSlotUnlock(slot_ptr, seq);
        cur = (cur + mask) & mask; /* previous slot */
    }
}

/* ── CLOCK eviction ─────────────────────────────────────────────────────── */

/*
 * Bounded CLOCK sweep.  Advances the shared hand, gives referenced slots
 * a second chance, evicts the first unreferenced occupied slot found.
 *
 * Pass 1 is bounded to 2× capacity (one pass to clear referenced bits, one
 * pass to evict).  If every slot seen was referenced, a second bounded pass
 * force-evicts the first occupied slot — approximate LRU is allowed to evict
 * a warm entry, it is not allowed to hang.  If the sweep sees no occupied
 * slots at all, it returns NULL rather than spinning on a desynced count.
 *
 * On contention (try-lock fails) the hand moves to the next slot rather
 * than stalling — correct for the CLOCK heuristic, which is approximate.
 */
LruClientData *
sClockEvict(LocklessLru *lru_ptr)
{
    const size_t cap   = lru_ptr->capacity;
    const size_t limit = cap * (size_t)PRIV_CONFIG_LOCKLESS_LRU_CLOCK_SWEEP_MULT;

    size_t occupied_seen = 0;

    for (size_t step = 0; step < limit; step++) {
        size_t hand = atomic_fetch_add_explicit(&lru_ptr->clock_hand, 1,
                                                memory_order_relaxed)
                      % cap;
        LruSlot *slot_ptr = &lru_ptr->slots[hand];

        uint32_t seq;
        if (!sSlotTryLock(slot_ptr, &seq)) continue; /* contended, try next hand */

        uint8_t state = atomic_load_explicit(&slot_ptr->state, memory_order_relaxed);
        if (state != CDT_LOCKLESS_LRU_SLOT_OCCUPIED) {
            sSlotUnlock(slot_ptr, seq);
            continue;
        }

        occupied_seen++;

        if (atomic_load_explicit(&slot_ptr->referenced, memory_order_relaxed)) {
            atomic_store_explicit(&slot_ptr->referenced, false, memory_order_relaxed);
            sSlotUnlock(slot_ptr, seq);
            continue; /* second chance given, move on */
        }

        LruClientData *evicted_ptr = atomic_load_explicit(&slot_ptr->data,
                                                          memory_order_relaxed);
        uint64_t gen = atomic_load_explicit(&slot_ptr->gen, memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->state,
                              (uint8_t)CDT_LOCKLESS_LRU_SLOT_TOMBSTONE,
                              memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->data, NULL, memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->gen, gen + 1, memory_order_relaxed);
        sSlotUnlock(slot_ptr, seq);

        sCountSub(lru_ptr);
        sTryCollapseTombstone(lru_ptr, hand);
        return evicted_ptr;
    }

    /* Every occupied slot seen was referenced — force-evict the first one
     * found on a second bounded walk. */
    if (occupied_seen > 0) {
        for (size_t step = 0; step < cap; step++) {
            size_t hand = atomic_fetch_add_explicit(&lru_ptr->clock_hand, 1,
                                                    memory_order_relaxed)
                          % cap;
            LruSlot *slot_ptr = &lru_ptr->slots[hand];
            uint32_t seq;
            if (!sSlotTryLock(slot_ptr, &seq)) continue;
            uint8_t state = atomic_load_explicit(&slot_ptr->state, memory_order_relaxed);
            if (state != CDT_LOCKLESS_LRU_SLOT_OCCUPIED) {
                sSlotUnlock(slot_ptr, seq);
                continue;
            }
            LruClientData *evicted_ptr = atomic_load_explicit(&slot_ptr->data,
                                                              memory_order_relaxed);
            uint64_t gen = atomic_load_explicit(&slot_ptr->gen, memory_order_relaxed);
            atomic_store_explicit(&slot_ptr->state,
                                  (uint8_t)CDT_LOCKLESS_LRU_SLOT_TOMBSTONE,
                                  memory_order_relaxed);
            atomic_store_explicit(&slot_ptr->data, NULL, memory_order_relaxed);
            atomic_store_explicit(&slot_ptr->gen, gen + 1, memory_order_relaxed);
            sSlotUnlock(slot_ptr, seq);
            sCountSub(lru_ptr);
            sTryCollapseTombstone(lru_ptr, hand);
            return evicted_ptr;
        }
    }

    return NULL; /* no occupied slots — count desync or race; spinning can't help */
}

/* ── Public API ─────────────────────────────────────────────────────────── */

PUBLIC_API LocklessLru *
LocklessLruCreateWithLogger(const LocklessLruConfig *config_ptr, UfLogger *logger_ptr)
{
    size_t capacity_hint = config_ptr ? config_ptr->capacity_hint : 0;
    if (capacity_hint == 0)
        capacity_hint = CONFIG_DEFAULT_LOCKLESS_LRU_CAPACITY_HINT;

    /* Each rejection below is reported separately: they all used to reach the
       caller as the same bare NULL, with nothing to tell them apart. */
    const size_t mult = CONFIG_DEFAULT_LOCKLESS_LRU_PROBE_HEADROOM_MULT;
    if (mult != 0 && capacity_hint > SIZE_MAX / mult) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "LRU cache rejected: capacity_hint %zu overflows with headroom multiplier %zu",
                            capacity_hint, mult);
        }
        return NULL; /* capacity_hint * headroom overflows size_t */
    }

    size_t min_phys = capacity_hint * mult;
    if (min_phys < PRIV_CONFIG_DEFAULT_LOCKLESS_LRU_MIN_SLOTS)
        min_phys = PRIV_CONFIG_DEFAULT_LOCKLESS_LRU_MIN_SLOTS;

    size_t phys_capacity = sNextPow2(min_phys);
    if (phys_capacity == 0) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "LRU cache rejected: slot count %zu is unrepresentable", min_phys);
        }
        return NULL;
    }
    if (phys_capacity > SIZE_MAX / sizeof(LruSlot)) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "LRU cache rejected: %zu slots overflow the slot table size",
                            phys_capacity);
        }
        return NULL;
    }

    LocklessLru *lru_ptr = calloc(1, sizeof(LocklessLru));
    if (!lru_ptr) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr, "LRU cache handle allocation failed");
        }
        return NULL;
    }

    /* Set the borrow first, so every failure below can be reported through it. */
    lru_ptr->uf_logger = logger_ptr;

    /* aligned_alloc guarantees 64-byte alignment so every LruSlot sits on
     * its own cache line — no slot ever crosses a cache-line boundary. */
    lru_ptr->slots = aligned_alloc(64, phys_capacity * sizeof(LruSlot));
    if (!lru_ptr->slots) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "LRU cache slot table allocation failed: %zu slots of %zu byte(s)",
                            phys_capacity, sizeof(LruSlot));
        }
        free(lru_ptr);
        return NULL;
    }
    memset(lru_ptr->slots, 0, phys_capacity * sizeof(LruSlot));

    lru_ptr->capacity  = phys_capacity;
    lru_ptr->max_items = capacity_hint;
    atomic_init(&lru_ptr->clock_hand, 0);
    atomic_init(&lru_ptr->count, 0);

    for (size_t i = 0; i < phys_capacity; i++) {
        atomic_init(&lru_ptr->slots[i].seq, 0u);
        atomic_init(&lru_ptr->slots[i].state,
                    (uint8_t)CDT_LOCKLESS_LRU_SLOT_EMPTY);
        atomic_init(&lru_ptr->slots[i].referenced, false);
        atomic_init(&lru_ptr->slots[i].key, (uint64_t)0);
        atomic_init(&lru_ptr->slots[i].data, (LruClientData *)NULL);
        atomic_init(&lru_ptr->slots[i].gen, (uint64_t)0);
    }

    if (logger_ptr != NULL) {
        UF_LOGGER_DEBUG(logger_ptr,
                        "LRU cache created: max_items=%zu physical_slots=%zu",
                        capacity_hint, phys_capacity);
    }
    return lru_ptr;
}

PUBLIC_API LocklessLru *
LocklessLruCreate(const LocklessLruConfig *config_ptr)
{
    return LocklessLruCreateWithLogger(config_ptr, NULL);
}

PUBLIC_API void
LocklessLruDestroy(LocklessLru *lru_ptr)
{
    UfLogger *logger_ptr;
    size_t    resident;

    if (!lru_ptr) return;

    /* Read the borrow before anything is released: it lives in the handle that
       is about to be freed. */
    logger_ptr = lru_ptr->uf_logger;

    /* Unlike a queue, a cache is expected to be holding entries at teardown, so
       this is reported at DEBUG rather than as a warning: it is a sizing
       datapoint (was max_items ever the binding constraint?), not an anomaly. */
    resident = atomic_load_explicit(&lru_ptr->count, memory_order_relaxed);

    free(lru_ptr->slots);
    lru_ptr->slots = NULL;

    if (logger_ptr != NULL) {
        UF_LOGGER_DEBUG(logger_ptr,
                        "LRU cache destroyed: %zu of %zu item(s) still resident",
                        resident, lru_ptr->max_items);
    }
    free(lru_ptr);
}

static LocklessLruRef
sGetCommon(LocklessLru *lru_ptr, uint64_t key)
{
    LocklessLruRef ref = { .data = NULL, .gen = 0 };
    if (!lru_ptr) return ref;

    size_t mask = lru_ptr->capacity - 1;
    size_t idx  = sMix64(key) & mask;

    for (size_t probe = 0; probe < lru_ptr->capacity; probe++) {
        LruSlot *slot_ptr = &lru_ptr->slots[(idx + probe) & mask];
        uint8_t state; uint64_t skey; LruClientData *sdata; uint64_t gen;
        sSlotRead(slot_ptr, &state, &skey, &sdata, &gen);

        if (state == CDT_LOCKLESS_LRU_SLOT_EMPTY) return ref; /* end of probe chain */
        if (state == CDT_LOCKLESS_LRU_SLOT_OCCUPIED && skey == key) {
            atomic_store_explicit(&slot_ptr->referenced, true, memory_order_relaxed);
            ref.data = sdata;
            ref.gen  = gen;
            return ref;
        }
        /* TOMBSTONE, or OCCUPIED-with-different-key: keep probing */
    }
    return ref;
}

PUBLIC_API LruClientData *
LocklessLruGet(LocklessLru *lru_ptr, uint64_t key)
{
    return sGetCommon(lru_ptr, key).data;
}

PUBLIC_API LocklessLruRef
LocklessLruGetRef(LocklessLru *lru_ptr, uint64_t key)
{
    return sGetCommon(lru_ptr, key);
}

PUBLIC_API bool
LocklessLruRefStillValid(LocklessLru *lru_ptr, uint64_t key, uint64_t gen)
{
    if (!lru_ptr) return false;
    size_t mask = lru_ptr->capacity - 1;
    size_t idx  = sMix64(key) & mask;

    for (size_t probe = 0; probe < lru_ptr->capacity; probe++) {
        LruSlot *slot_ptr = &lru_ptr->slots[(idx + probe) & mask];
        uint8_t state; uint64_t skey; LruClientData *sdata; uint64_t sgen;
        sSlotRead(slot_ptr, &state, &skey, &sdata, &sgen);
        if (state == CDT_LOCKLESS_LRU_SLOT_EMPTY) return false;
        if (state == CDT_LOCKLESS_LRU_SLOT_OCCUPIED && skey == key)
            return sgen == gen;
    }
    return false;
}

PUBLIC_API LocklessLruSetResult
LocklessLruSetEx(LocklessLru *lru_ptr, uint64_t key, LruClientData *data_ptr)
{
    LocklessLruSetResult out = { .status = LOCKLESS_LRU_SET_FULL, .displaced = NULL };
    if (!lru_ptr) return out;

    size_t mask = lru_ptr->capacity - 1;
    size_t idx  = sMix64(key) & mask;

    /* Pass 1: find an existing slot for this key (upsert) and remember the
     * first reusable hole (EMPTY or TOMBSTONE) in case it is an insert. */
    size_t hole = (size_t)-1;
    int    have_hole = 0;

    for (size_t probe = 0; probe < lru_ptr->capacity; probe++) {
        size_t at = (idx + probe) & mask;
        LruSlot *slot_ptr = &lru_ptr->slots[at];
        uint8_t state; uint64_t skey;
        sSlotPeek(slot_ptr, &state, &skey);

        if (state == CDT_LOCKLESS_LRU_SLOT_OCCUPIED && skey == key) {
            uint32_t seq = sSlotLock(slot_ptr);
            uint8_t  st2 = atomic_load_explicit(&slot_ptr->state, memory_order_relaxed);
            uint64_t k2  = atomic_load_explicit(&slot_ptr->key, memory_order_relaxed);
            if (st2 == CDT_LOCKLESS_LRU_SLOT_OCCUPIED && k2 == key) {
                LruClientData *old = atomic_load_explicit(&slot_ptr->data,
                                                          memory_order_relaxed);
                uint64_t gen = atomic_load_explicit(&slot_ptr->gen, memory_order_relaxed);
                atomic_store_explicit(&slot_ptr->data, data_ptr, memory_order_relaxed);
                atomic_store_explicit(&slot_ptr->referenced, true, memory_order_relaxed);
                atomic_store_explicit(&slot_ptr->gen, gen + 1, memory_order_relaxed);
                sSlotUnlock(slot_ptr, seq);
                out.status    = LOCKLESS_LRU_SET_REPLACED;
                out.displaced = old;
                return out;
            }
            sSlotUnlock(slot_ptr, seq);
            /* Lost the key; keep scanning / fall through to insert. */
            continue;
        }

        if (!have_hole &&
            (state == CDT_LOCKLESS_LRU_SLOT_EMPTY ||
             state == CDT_LOCKLESS_LRU_SLOT_TOMBSTONE)) {
            hole = at;
            have_hole = 1;
        }
        if (state == CDT_LOCKLESS_LRU_SLOT_EMPTY)
            break;
    }

    if (!have_hole) {
        out.status = LOCKLESS_LRU_SET_FULL;
        return out;
    }

    /* Pass 2: claim the remembered hole first, then any later reusable slot. */
    for (size_t probe = 0; probe < lru_ptr->capacity; probe++) {
        size_t at = (have_hole && probe == 0) ? hole : ((idx + probe) & mask);
        LruSlot *slot_ptr = &lru_ptr->slots[at];

        uint8_t state; uint64_t skey;
        sSlotPeek(slot_ptr, &state, &skey);
        if (state == CDT_LOCKLESS_LRU_SLOT_OCCUPIED) {
            if (skey == key) {
                /* Appeared meanwhile — replace it in place. */
                uint32_t seq = sSlotLock(slot_ptr);
                if (atomic_load_explicit(&slot_ptr->state, memory_order_relaxed) ==
                        CDT_LOCKLESS_LRU_SLOT_OCCUPIED &&
                    atomic_load_explicit(&slot_ptr->key, memory_order_relaxed) == key) {
                    LruClientData *old = atomic_load_explicit(&slot_ptr->data,
                                                              memory_order_relaxed);
                    uint64_t gen = atomic_load_explicit(&slot_ptr->gen, memory_order_relaxed);
                    atomic_store_explicit(&slot_ptr->data, data_ptr, memory_order_relaxed);
                    atomic_store_explicit(&slot_ptr->referenced, true, memory_order_relaxed);
                    atomic_store_explicit(&slot_ptr->gen, gen + 1, memory_order_relaxed);
                    sSlotUnlock(slot_ptr, seq);
                    out.status    = LOCKLESS_LRU_SET_REPLACED;
                    out.displaced = old;
                    return out;
                }
                sSlotUnlock(slot_ptr, seq);
            }
            have_hole = 0;
            continue;
        }

        uint32_t seq = sSlotLock(slot_ptr);
        uint8_t st2 = atomic_load_explicit(&slot_ptr->state, memory_order_relaxed);
        if (st2 == CDT_LOCKLESS_LRU_SLOT_OCCUPIED) {
            uint64_t k2 = atomic_load_explicit(&slot_ptr->key, memory_order_relaxed);
            if (k2 == key) {
                LruClientData *old = atomic_load_explicit(&slot_ptr->data,
                                                          memory_order_relaxed);
                uint64_t gen = atomic_load_explicit(&slot_ptr->gen, memory_order_relaxed);
                atomic_store_explicit(&slot_ptr->data, data_ptr, memory_order_relaxed);
                atomic_store_explicit(&slot_ptr->referenced, true, memory_order_relaxed);
                atomic_store_explicit(&slot_ptr->gen, gen + 1, memory_order_relaxed);
                sSlotUnlock(slot_ptr, seq);
                out.status    = LOCKLESS_LRU_SET_REPLACED;
                out.displaced = old;
                return out;
            }
            sSlotUnlock(slot_ptr, seq);
            have_hole = 0;
            continue;
        }

        uint64_t gen = atomic_load_explicit(&slot_ptr->gen, memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->key, key, memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->data, data_ptr, memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->referenced, true, memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->gen, gen + 1, memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->state,
                              (uint8_t)CDT_LOCKLESS_LRU_SLOT_OCCUPIED,
                              memory_order_relaxed);
        sSlotUnlock(slot_ptr, seq);

        atomic_fetch_add_explicit(&lru_ptr->count, 1, memory_order_relaxed);

        /* Insert-then-trim: evict only if the insert pushed count past the
         * soft target. */
        LruClientData *victim = NULL;
        if (atomic_load_explicit(&lru_ptr->count, memory_order_relaxed) > lru_ptr->max_items)
            victim = sClockEvict(lru_ptr);

        if (victim) {
            out.status    = LOCKLESS_LRU_SET_EVICTED;
            out.displaced = victim;
        } else {
            out.status    = LOCKLESS_LRU_SET_INSERTED;
            out.displaced = NULL;
        }
        return out;
    }

    out.status = LOCKLESS_LRU_SET_FULL;
    return out;
}

PUBLIC_API LruClientData *
LocklessLruSet(LocklessLru *lru_ptr, uint64_t key, LruClientData *data_ptr)
{
    return LocklessLruSetEx(lru_ptr, key, data_ptr).displaced;
}

PUBLIC_API LruClientData *
LocklessLruRemove(LocklessLru *lru_ptr, uint64_t key)
{
    if (!lru_ptr) return NULL;
    size_t mask = lru_ptr->capacity - 1;
    size_t idx  = sMix64(key) & mask;

    for (size_t probe = 0; probe < lru_ptr->capacity; probe++) {
        size_t at = (idx + probe) & mask;
        LruSlot *slot_ptr = &lru_ptr->slots[at];

        uint8_t state; uint64_t skey;
        sSlotPeek(slot_ptr, &state, &skey);

        if (state == CDT_LOCKLESS_LRU_SLOT_EMPTY) return NULL;
        if (state != CDT_LOCKLESS_LRU_SLOT_OCCUPIED || skey != key) continue;

        uint32_t seq = sSlotLock(slot_ptr);
        if (atomic_load_explicit(&slot_ptr->state, memory_order_relaxed) !=
                CDT_LOCKLESS_LRU_SLOT_OCCUPIED ||
            atomic_load_explicit(&slot_ptr->key, memory_order_relaxed) != key) {
            sSlotUnlock(slot_ptr, seq); /* changed between peek and lock */
            continue;
        }

        LruClientData *removed_ptr = atomic_load_explicit(&slot_ptr->data,
                                                          memory_order_relaxed);
        uint64_t gen = atomic_load_explicit(&slot_ptr->gen, memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->state,
                              (uint8_t)CDT_LOCKLESS_LRU_SLOT_TOMBSTONE,
                              memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->data, NULL, memory_order_relaxed);
        atomic_store_explicit(&slot_ptr->gen, gen + 1, memory_order_relaxed);
        sSlotUnlock(slot_ptr, seq);

        sCountSub(lru_ptr);
        sTryCollapseTombstone(lru_ptr, at);
        return removed_ptr;
    }
    return NULL;
}

PUBLIC_API size_t
LocklessLruSize(LocklessLru *lru_ptr)
{
    if (!lru_ptr) return 0;
    return atomic_load_explicit(&lru_ptr->count, memory_order_acquire);
}

PUBLIC_API size_t
LocklessLruCapacity(LocklessLru *lru_ptr)
{
    if (!lru_ptr) return 0;
    return lru_ptr->capacity;
}

PUBLIC_API BufferDescriptor *
DescribeLocklessLru(LocklessLru *lru_ptr, BufferDescriptor *provided)
{
    if (!provided) {
        provided = calloc(1, sizeof(BufferDescriptor));
        if (!provided) return NULL;
        BufferDescriptorInit(provided, 256);
    }

    if (!lru_ptr) {
        BufferDescriptorAppendFormatted(provided, "{\"error\":\"null handle\"}\n");
        return provided;
    }

    size_t capacity   = lru_ptr->capacity;
    size_t max_items  = lru_ptr->max_items;
    size_t count      = atomic_load_explicit(&lru_ptr->count, memory_order_acquire);
    size_t clock_hand = atomic_load_explicit(&lru_ptr->clock_hand, memory_order_relaxed);

    /* Pass 1 — best-effort occupancy snapshot (relaxed loads; approximate under
     * concurrent mutation). */
    size_t occupied = 0, empty = 0, tombstone = 0, referenced = 0;
    for (size_t i = 0; i < capacity; i++) {
        uint8_t state = atomic_load_explicit(&lru_ptr->slots[i].state, memory_order_relaxed);
        if (state == CDT_LOCKLESS_LRU_SLOT_OCCUPIED) {
            occupied++;
            if (atomic_load_explicit(&lru_ptr->slots[i].referenced, memory_order_relaxed))
                referenced++;
        } else if (state == CDT_LOCKLESS_LRU_SLOT_EMPTY) {
            empty++;
        } else {
            tombstone++; /* TOMBSTONE */
        }
    }

    double load_factor = (capacity > 0) ? (double)occupied / (double)capacity : 0.0;

    /* Whether this cache reports anywhere.  Emitted because "no diagnostics
       appeared" and "no diagnostics were configured" are otherwise
       indistinguishable from the outside, and they call for different fixes. */
    const char *logger_state = lru_ptr->uf_logger != NULL ? "enabled" : "none";

    BufferDescriptorAppendFormatted(provided,
        "{\"capacity\":%zu,\"max_items\":%zu,\"count\":%zu,\"clock_hand\":%zu,"
        "\"logger\":\"%s\","
        "\"load_factor\":%.4f,"
        "\"occupancy\":{\"occupied\":%zu,\"empty\":%zu,\"tombstone\":%zu,"
        "\"referenced\":%zu}",
        capacity, max_items, count, clock_hand, logger_state,
        load_factor, occupied, empty, tombstone, referenced);

    /* Pass 2 — one entry per occupied slot. */
    BufferDescriptorAppendFormatted(provided, ",\"slots\":[");
    bool emitted = false;
    for (size_t i = 0; i < capacity; i++) {
        uint8_t state = atomic_load_explicit(&lru_ptr->slots[i].state, memory_order_relaxed);
        if (state != CDT_LOCKLESS_LRU_SLOT_OCCUPIED) continue;
        uint64_t key = atomic_load_explicit(&lru_ptr->slots[i].key, memory_order_relaxed);
        bool     ref = atomic_load_explicit(&lru_ptr->slots[i].referenced, memory_order_relaxed);
        uint64_t gen = atomic_load_explicit(&lru_ptr->slots[i].gen, memory_order_relaxed);
        BufferDescriptorAppendFormatted(provided,
            "%s{\"index\":%zu,\"key\":%" PRIu64 ",\"referenced\":%s,\"gen\":%" PRIu64 "}",
            emitted ? "," : "", i, key, ref ? "true" : "false", gen);
        emitted = true;
    }
    BufferDescriptorAppendFormatted(provided, "]}\n");

    return provided;
}
