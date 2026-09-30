/**
 * @file cdt_lockless_minheap.c
 * @brief Lock-free integer min-priority queue — Harris lock-free ordered list.
 *
 * The queue is a sorted singly-linked list of nodes keyed by inline int64_t.
 * insert_i64 links a new node after its predecessor (release CAS); delmin_i64
 * marks the minimum's successor (acq_rel CAS) and then physically unlinks it.
 * Marking is the linearization point; unlink is cooperative — whichever
 * walker sees a marked node unlinks and retires it, so each node is retired
 * exactly once.
 *
 * Retired nodes are pushed onto an intrusive LocklessTreiberStack.  The
 * refcount contract follows the stack's documented model: the retiring thread
 * drops the "pusher" reference after push (2 → 1), and the drainer drops the
 * final "stack" reference (1 → 0) during reclaim/destroy, freeing each node
 * exactly once.
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

#include "cdt_lockless_minheap_priv.h"

#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack.h>
#include <uflib/logger/logger.h>

#include <inttypes.h>
#include <stdlib.h>

/* ── Mark helpers (LSB tagging on an 8-byte-aligned node pointer) ────────── */

static struct LocklessMinHeapNode *
sMark(struct LocklessMinHeapNode *p)
{
    return (struct LocklessMinHeapNode *)((uintptr_t)p | (uintptr_t)1);
}

static struct LocklessMinHeapNode *
sUnmark(struct LocklessMinHeapNode *p)
{
    return (struct LocklessMinHeapNode *)((uintptr_t)p & ~(uintptr_t)1);
}

static struct LocklessMinHeapNode *
sLoadNext(struct LocklessMinHeapNode *n, memory_order mo)
{
    return atomic_load_explicit(&n->next, mo);
}

/* ── Node lifecycle ─────────────────────────────────────────────────────── */

static struct LocklessMinHeapNode *
sMakeNode(int64_t key, void *value)
{
    struct LocklessMinHeapNode *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    lockless_treiber_stack_node_init(&n->retire);
    atomic_store_explicit(&n->next, NULL, memory_order_relaxed);
    n->key = key;
    n->value = value;
    return n;
}

/*!
 * Recover the owning queue node from its embedded retire link.  Safe because
 * `retire` is the first member (offset 0, locked by _Static_assert).
 */
static struct LocklessMinHeapNode *
sContainer(struct LocklessTreiberStackNode *r)
{
    return (struct LocklessMinHeapNode *)((char *)r - offsetof(struct LocklessMinHeapNode, retire));
}

/*!
 * Hand a logically-deleted node to the retire stack.  Re-initialises the
 * intrusive node (idempotent), pushes it, then drops the "pusher" reference
 * (2 → 1).  The final "stack" reference is dropped by the drainer.
 */
static void
sRetireNode(struct LocklessMinHeap *h, struct LocklessMinHeapNode *n)
{
    lockless_treiber_stack_node_init(&n->retire);
    lockless_treiber_stack_push(h->retired, &n->retire);
    (void)lockless_treiber_stack_release(&n->retire);
}

/*!
 * Free a chain stolen from the retire stack.  The chain is exclusively owned
 * by the drainer, so a single release per node reaches zero and frees it.
 */
static void
sFreeStolen(struct LocklessTreiberStackNode *list)
{
    while (list) {
        struct LocklessTreiberStackNode *next =
            atomic_load_explicit(&list->next, memory_order_relaxed);
        if (lockless_treiber_stack_release(list))
            free(sContainer(list));
        list = next;
    }
}

/* ── Window search (Harris traversal with helping) ──────────────────────── */

/*
 * Find the window (pred, curr) where curr is the first live node with
 * key >= *search_key, physically unlinking any marked node along the way
 * (cooperative helping).  When search_key is NULL this yields the first live
 * node — the current minimum — for delmin.
 */
static void
sFindWindow(struct LocklessMinHeap *h, const int64_t *search_key,
            struct LocklessMinHeapNode **pred_out,
            struct LocklessMinHeapNode **curr_out)
{
    for (;;) {
        struct LocklessMinHeapNode *pred = h->head;
        struct LocklessMinHeapNode *curr = sUnmark(sLoadNext(pred, memory_order_acquire));

        for (;;) {
            if (!curr) {
                *pred_out = pred;
                *curr_out = NULL;
                return;
            }

            struct LocklessMinHeapNode *succ_raw = sLoadNext(curr, memory_order_acquire);
            struct LocklessMinHeapNode *succ = sUnmark(succ_raw);

            if (succ_raw != succ) {
                /* curr is marked — physically unlink and retire it (helping). */
                struct LocklessMinHeapNode *expected = curr;
                if (atomic_compare_exchange_strong_explicit(
                        &pred->next, &expected, succ,
                        memory_order_acq_rel, memory_order_acquire)) {
                    sRetireNode(h, curr);
                    curr = succ;
                    continue;
                }
                break; /* concurrent modification — restart from the head */
            }

            if (search_key == NULL || curr->key >= *search_key) {
                *pred_out = pred;
                *curr_out = curr;
                return;
            }

            pred = curr;
            curr = succ;
        }
    }
}

/* ── Size bookkeeping ───────────────────────────────────────────────────── */

static void
sSizeSub(struct LocklessMinHeap *h)
{
    /* Unconditional decrement: insert's increment is delayed relative to the
     * link (the node is visible before the fetch_add), so a concurrent delmin
     * can observe a transiently-low counter and must NOT skip its decrement —
     * skipping would leave a permanent overcount.  A signed counter absorbs
     * the transient underflow; reads clamp at 0. */
    (void)atomic_fetch_sub_explicit(&h->approx_size, 1, memory_order_relaxed);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

PUBLIC_API struct LocklessMinHeap *
LocklessMinHeapCreateWithLogger(UfLogger *logger_ptr)
{
    struct LocklessMinHeap *h = calloc(1, sizeof(*h));
    if (!h) {
        /* Previously this failure reached the caller as a bare NULL and nothing
           else: no reason, and no record that it happened. */
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr, "min-heap handle allocation failed");
        }
        return NULL;
    }

    /* Set the borrow before any sub-allocation, so every path below can report
       and so the handle is never briefly a logger-less half-built structure. */
    h->uf_logger = logger_ptr;

    /* The retire stack borrows the same logger: one handle covers the whole
       structure, so a diagnostic never depends on which half reported it. */
    h->retired = lockless_treiber_stack_create_with_logger(logger_ptr);
    if (!h->retired) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr, "min-heap retire stack allocation failed");
        }
        free(h);
        return NULL;
    }

    h->head = sMakeNode(INT64_MIN, NULL);
    if (!h->head) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr, "min-heap sentinel allocation failed");
        }
        lockless_treiber_stack_destroy(h->retired);
        free(h);
        return NULL;
    }

    atomic_store_explicit(&h->approx_size, 0, memory_order_relaxed);

    if (logger_ptr != NULL) {
        UF_LOGGER_DEBUG(logger_ptr, "min-heap created");
    }
    return h;
}

PUBLIC_API struct LocklessMinHeap *
LocklessMinHeapCreate(void)
{
    return LocklessMinHeapCreateWithLogger(NULL);
}

PUBLIC_API void
LocklessMinHeapDestroy(struct LocklessMinHeap *h)
{
    UfLogger *logger_ptr;

    if (!h)
        return;

    /* Read the borrow before anything is released: it lives in the handle that
       is about to be freed. */
    logger_ptr = h->uf_logger;

    /* Free the live chain.  Quiescent-only: no concurrent insert/delmin. */
    if (h->head) {
        struct LocklessMinHeapNode *n =
            sUnmark(sLoadNext(h->head, memory_order_relaxed));
        while (n) {
            struct LocklessMinHeapNode *next =
                sUnmark(sLoadNext(n, memory_order_relaxed));
            free(n);
            n = next;
        }
        free(h->head);
    }

    LocklessMinHeapReclaim(h);

    /* The retire stack borrows the same logger, so release it while the borrow
       is still valid; it reports its own release. */
    lockless_treiber_stack_destroy(h->retired);

    if (logger_ptr != NULL) {
        UF_LOGGER_DEBUG(logger_ptr, "min-heap destroyed");
    }
    free(h);
}

PUBLIC_API int
LocklessMinHeapInsertI64(struct LocklessMinHeap *h, int64_t key, void *value)
{
    if (!h)
        return LOCKLESS_MINHEAP_ERR_INVAL;

    struct LocklessMinHeapNode *node = sMakeNode(key, value);
    if (!node)
        return LOCKLESS_MINHEAP_ERR_OOM;

    for (;;) {
        struct LocklessMinHeapNode *pred, *curr;
        sFindWindow(h, &key, &pred, &curr);

        atomic_store_explicit(&node->next, curr, memory_order_relaxed);
        struct LocklessMinHeapNode *expected = curr;
        if (atomic_compare_exchange_strong_explicit(
                &pred->next, &expected, node,
                memory_order_release, memory_order_acquire)) {
            atomic_fetch_add_explicit(&h->approx_size, 1, memory_order_relaxed);
            return LOCKLESS_MINHEAP_OK;
        }
    }
}

PUBLIC_API int
LocklessMinHeapDelminI64(struct LocklessMinHeap *h, int64_t *key, void **value)
{
    if (!h)
        return 0;

    for (;;) {
        struct LocklessMinHeapNode *pred, *curr;
        sFindWindow(h, NULL, &pred, &curr);
        if (!curr)
            return 0;

        struct LocklessMinHeapNode *succ = sUnmark(sLoadNext(curr, memory_order_acquire));
        struct LocklessMinHeapNode *expected = succ;
        /* Linearization point: mark curr. */
        if (!atomic_compare_exchange_strong_explicit(
                &curr->next, &expected, sMark(succ),
                memory_order_acq_rel, memory_order_acquire)) {
            continue; /* lost the mark race — retry */
        }

        /* Physically unlink curr; a concurrent helper may beat us to it. */
        expected = curr;
        if (atomic_compare_exchange_strong_explicit(
                &pred->next, &expected, succ,
                memory_order_acq_rel, memory_order_acquire)) {
            sRetireNode(h, curr);
        }

        if (key)
            *key = curr->key;
        if (value)
            *value = curr->value;

        sSizeSub(h);
        return 1;
    }
}

PUBLIC_API size_t
LocklessMinHeapSizeApprox(const struct LocklessMinHeap *h)
{
    if (!h)
        return 0;
    int64_t sz = atomic_load_explicit(&h->approx_size, memory_order_relaxed);
    return sz > 0 ? (size_t)sz : 0;
}

PUBLIC_API void
LocklessMinHeapReclaim(struct LocklessMinHeap *h)
{
    if (!h || !h->retired)
        return;
    sFreeStolen(lockless_treiber_stack_steal_all(h->retired));
}

PUBLIC_API BufferDescriptor *
DescribeLocklessMinHeap(struct LocklessMinHeap *h, BufferDescriptor *provided)
{
    if (!provided) {
        provided = calloc(1, sizeof(BufferDescriptor));
        if (!provided)
            return NULL;
        BufferDescriptorInit(provided, 256);
    }

    if (!h) {
        BufferDescriptorAppendFormatted(provided, "{\"error\":\"null handle\"}\n");
        return provided;
    }

    int64_t approx_raw = atomic_load_explicit(&h->approx_size, memory_order_relaxed);
    size_t  approx = approx_raw > 0 ? (size_t)approx_raw : 0;
    size_t  live = 0;
    for (struct LocklessMinHeapNode *n = sUnmark(sLoadNext(h->head, memory_order_relaxed));
         n; n = sUnmark(sLoadNext(n, memory_order_relaxed))) {
        live++;
    }

    /* Whether this queue reports anywhere.  Emitted because "no diagnostics
       appeared" and "no diagnostics were configured" are otherwise
       indistinguishable from the outside, and they call for different fixes. */
    const char *logger_state = h->uf_logger != NULL ? "enabled" : "none";

    BufferDescriptorAppendFormatted(provided,
        "{\"approx_size\":%zu,\"live_nodes\":%zu,\"logger\":\"%s\",\"entries\":[",
        approx, live, logger_state);

    bool first = true;
    for (struct LocklessMinHeapNode *n = sUnmark(sLoadNext(h->head, memory_order_relaxed));
         n; n = sUnmark(sLoadNext(n, memory_order_relaxed))) {
        if (n->value)
            BufferDescriptorAppendFormatted(provided,
                "%s{\"key\":%" PRId64 ",\"value\":\"%p\"}",
                first ? "" : ",", n->key, n->value);
        else
            BufferDescriptorAppendFormatted(provided,
                "%s{\"key\":%" PRId64 ",\"value\":null}",
                first ? "" : ",", n->key);
        first = false;
    }
    BufferDescriptorAppendFormatted(provided, "]}\n");

    return provided;
}
