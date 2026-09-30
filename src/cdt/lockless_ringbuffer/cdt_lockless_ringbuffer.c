/**
 * @file cdt_lockless_ringbuffer.c
 * @brief Lock-free bounded ring buffer — SPSC two-index fast path and
 *        Vyukov per-slot sequence-number protocol for MPSC/MPMC.
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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include "cdt_lockless_ringbuffer_priv.h"

#include <uflib/logger/logger.h>

#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ───────────────────────────────────────────────────── */

/*
 * Smallest power of two >= n, or 0 if the next power of two would overflow
 * size_t (n > 2^(bits-1)).  n == 0 is not expected — the caller validates
 * capacity != 0 before invoking this.
 */
static size_t
sNextPowerOfTwo(size_t n)
{
    size_t p = 1;
    while (p < n) {
        if (p > (SIZE_MAX >> 1)) {
            return 0;   /* doubling p would overflow — unrepresentable */
        }
        p <<= 1;
    }
    return p;
}

/*
 * calloc with an explicit count * size overflow guard.  Relying on calloc's
 * own overflow detection is not enough: sanitizers (TSan, UBSan) flag the
 * overflowing multiplication before the library even runs.  Returning NULL
 * up front keeps the overflow path clean and observable in every build.
 */
static void *
sCallocChecked(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        return NULL;   /* count * size would overflow size_t */
    }
    return calloc(count, size);
}

/* ── SPSC (two-index, one reserved slot) ────────────────────────────────── */

static bool
sRingSpscPush(LocklessRingBuffer *ring_ptr, const void *elem_ptr)
{
    size_t tail = atomic_load_explicit(&ring_ptr->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&ring_ptr->head, memory_order_acquire);

    /* One slot is reserved as the full/empty sentinel, so the buffer is full
     * once tail - head reaches usable == capacity - 1. */
    if (tail - head >= ring_ptr->usable) {
        return false;
    }

    memcpy(ring_ptr->data + (tail & ring_ptr->mask) * ring_ptr->elem_size,
           elem_ptr, ring_ptr->elem_size);
    atomic_store_explicit(&ring_ptr->tail, tail + 1, memory_order_release);
    return true;
}

static bool
sRingSpscPop(LocklessRingBuffer *ring_ptr, void *elem_ptr)
{
    size_t head = atomic_load_explicit(&ring_ptr->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&ring_ptr->tail, memory_order_acquire);

    if (head == tail) {
        return false;   /* empty */
    }

    memcpy(elem_ptr, ring_ptr->data + (head & ring_ptr->mask) * ring_ptr->elem_size,
           ring_ptr->elem_size);
    atomic_store_explicit(&ring_ptr->head, head + 1, memory_order_release);
    return true;
}

/* ── MPSC / MPMC (Vyukov per-slot sequence numbers) ─────────────────────── */

/*
 * The per-slot sequence number seq[i] encodes slot i's state.  For a slot
 * at position pos (so i == pos & mask) the states are:
 *
 *   seq[i] == pos      — slot empty, ready to be pushed at position pos.
 *   seq[i] == pos + 1  — slot full, ready to be popped at position pos.
 *
 * After a pop at position pos, seq[i] becomes pos + capacity, which for the
 * next cycle's position pos' = pos + capacity is exactly pos' — so the slot
 * reads as empty again.  Producers advance `tail` and consumers advance
 * `head` via CAS; the cursor CAS only arbitrates claims (relaxed) while the
 * release/acquire on seq[i] carries the data happens-before.
 */
static bool
sRingMpmcPush(LocklessRingBuffer *ring_ptr, const void *elem_ptr)
{
    size_t pos = atomic_load_explicit(&ring_ptr->tail, memory_order_relaxed);

    for (;;) {
        size_t idx = pos & ring_ptr->mask;
        size_t seq = atomic_load_explicit(&ring_ptr->seq[idx], memory_order_acquire);

        if (seq == pos) {
            /* Slot idx is empty — try to claim position pos. */
            if (atomic_compare_exchange_weak_explicit(
                    &ring_ptr->tail, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                memcpy(ring_ptr->data + idx * ring_ptr->elem_size,
                       elem_ptr, ring_ptr->elem_size);
                atomic_store_explicit(&ring_ptr->seq[idx], pos + 1,
                                      memory_order_release);
                return true;
            }
            /* CAS failed: another producer claimed pos; `pos` now holds the
             * current tail and the loop re-reads seq for the new position. */
        } else if (seq < pos) {
            /* Slot idx is still occupied (not yet consumed) — buffer full. */
            return false;
        } else {
            /* seq > pos: a producer already advanced tail past pos — re-read. */
            pos = atomic_load_explicit(&ring_ptr->tail, memory_order_relaxed);
        }
    }
}

static bool
sRingMpmcPop(LocklessRingBuffer *ring_ptr, void *elem_ptr)
{
    size_t pos = atomic_load_explicit(&ring_ptr->head, memory_order_relaxed);

    for (;;) {
        size_t idx = pos & ring_ptr->mask;
        size_t seq = atomic_load_explicit(&ring_ptr->seq[idx], memory_order_acquire);

        if (seq == pos + 1) {
            /* Slot idx is full — try to claim position pos. */
            if (atomic_compare_exchange_weak_explicit(
                    &ring_ptr->head, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                memcpy(elem_ptr, ring_ptr->data + idx * ring_ptr->elem_size,
                       ring_ptr->elem_size);
                atomic_store_explicit(&ring_ptr->seq[idx],
                                      pos + ring_ptr->capacity,
                                      memory_order_release);
                return true;
            }
            /* CAS failed: another consumer claimed pos — loop re-reads. */
        } else if (seq < pos + 1) {
            /* Slot idx not yet filled — buffer empty. */
            return false;
        } else {
            pos = atomic_load_explicit(&ring_ptr->head, memory_order_relaxed);
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

LocklessRingBuffer *
LocklessRingBufferCreateWithLogger(size_t capacity, size_t elem_size, LocklessRingBufferMode mode,
                                   UfLogger *logger_ptr)
{
    if (capacity == 0 || elem_size == 0) {
        /* Each rejection below is reported separately: they all used to reach
           the caller as the same bare NULL, with nothing to tell them apart. */
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "ring buffer rejected: capacity=%zu elem_size=%zu (both must be non-zero)",
                            capacity, elem_size);
        }
        return NULL;
    }
    if (mode != LOCKLESS_RINGBUF_MODE_SPSC &&
        mode != LOCKLESS_RINGBUF_MODE_MPSC &&
        mode != LOCKLESS_RINGBUF_MODE_MPMC) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "ring buffer rejected: mode %d is not SPSC, MPSC or MPMC", (int)mode);
        }
        return NULL;
    }

    capacity = sNextPowerOfTwo(capacity);
    if (capacity == 0) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "ring buffer rejected: requested capacity is unrepresentable");
        }
        return NULL;   /* requested size unrepresentable */
    }

    LocklessRingBuffer *ring_ptr = calloc(1, sizeof(*ring_ptr));
    if (!ring_ptr) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr, "ring buffer handle allocation failed");
        }
        return NULL;
    }

    /* Set the borrow first, so every failure below can be reported through it. */
    ring_ptr->uf_logger = logger_ptr;

    ring_ptr->capacity  = capacity;
    ring_ptr->usable    = (mode == LOCKLESS_RINGBUF_MODE_SPSC) ? capacity - 1 : capacity;
    ring_ptr->mask      = capacity - 1;
    ring_ptr->elem_size = elem_size;
    ring_ptr->mode      = mode;

    atomic_init(&ring_ptr->tail, 0);
    atomic_init(&ring_ptr->head, 0);

    ring_ptr->data = sCallocChecked(capacity, elem_size);
    if (!ring_ptr->data) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "ring buffer storage allocation failed: capacity=%zu elem_size=%zu",
                            capacity, elem_size);
        }
        free(ring_ptr);
        return NULL;
    }

    if (mode != LOCKLESS_RINGBUF_MODE_SPSC) {
        ring_ptr->seq = sCallocChecked(capacity, sizeof(_Atomic(size_t)));
        if (!ring_ptr->seq) {
            if (logger_ptr != NULL) {
                UF_LOGGER_ERROR(logger_ptr,
                                "ring buffer sequence allocation failed: capacity=%zu", capacity);
            }
            free(ring_ptr->data);
            free(ring_ptr);
            return NULL;
        }
        for (size_t i = 0; i < capacity; i++) {
            atomic_init(&ring_ptr->seq[i], i);
        }
    }

    if (logger_ptr != NULL) {
        UF_LOGGER_DEBUG(logger_ptr,
                        "ring buffer created: capacity=%zu elem_size=%zu mode=%d",
                        capacity, elem_size, (int)mode);
    }
    return ring_ptr;
}

LocklessRingBuffer *
LocklessRingBufferCreate(size_t capacity, size_t elem_size, LocklessRingBufferMode mode)
{
    return LocklessRingBufferCreateWithLogger(capacity, elem_size, mode, NULL);
}

void
LocklessRingBufferDestroy(LocklessRingBuffer *ring_ptr)
{
    UfLogger *logger_ptr;
    size_t    unconsumed;

    if (!ring_ptr) return;

    /* Read the borrow before anything is released: it lives in the handle that
       is about to be freed. */
    logger_ptr = ring_ptr->uf_logger;

    /* Quiescent by contract, so this read is exact rather than the approximate
       one the live accessor returns, and it is taken before the storage it
       describes is released.  Elements still in the buffer at teardown were
       never consumed — the one leak this structure can have, and previously an
       invisible one. */
    unconsumed = LocklessRingBufferSize(ring_ptr);

    free(ring_ptr->seq);
    free(ring_ptr->data);

    if (logger_ptr != NULL) {
        if (unconsumed != 0) {
            UF_LOGGER_WARN(logger_ptr,
                           "ring buffer destroyed with %zu unconsumed element(s) of %zu byte(s)",
                           unconsumed, ring_ptr->elem_size);
        } else {
            UF_LOGGER_DEBUG(logger_ptr, "ring buffer destroyed: empty, capacity=%zu",
                            ring_ptr->capacity);
        }
    }
    free(ring_ptr);
}

bool
LocklessRingBufferTryPush(LocklessRingBuffer *ring_ptr, const void *elem_ptr)
{
    if (!ring_ptr || !elem_ptr) return false;

    switch (ring_ptr->mode) {
        case LOCKLESS_RINGBUF_MODE_SPSC:
            return sRingSpscPush(ring_ptr, elem_ptr);
        case LOCKLESS_RINGBUF_MODE_MPSC:
        case LOCKLESS_RINGBUF_MODE_MPMC:
            return sRingMpmcPush(ring_ptr, elem_ptr);
        default:
            return false;   /* unreachable: mode validated at create */
    }
}

bool
LocklessRingBufferTryPop(LocklessRingBuffer *ring_ptr, void *elem_ptr)
{
    if (!ring_ptr || !elem_ptr) return false;

    switch (ring_ptr->mode) {
        case LOCKLESS_RINGBUF_MODE_SPSC:
            return sRingSpscPop(ring_ptr, elem_ptr);
        case LOCKLESS_RINGBUF_MODE_MPSC:
        case LOCKLESS_RINGBUF_MODE_MPMC:
            return sRingMpmcPop(ring_ptr, elem_ptr);
        default:
            return false;   /* unreachable: mode validated at create */
    }
}

size_t
LocklessRingBufferSize(const LocklessRingBuffer *ring_ptr)
{
    if (!ring_ptr) return 0;

    size_t head = atomic_load_explicit(&ring_ptr->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&ring_ptr->tail, memory_order_acquire);
    return tail - head;
}

size_t
LocklessRingBufferCapacity(const LocklessRingBuffer *ring_ptr)
{
    return ring_ptr ? ring_ptr->usable : 0;
}

bool
LocklessRingBufferEmpty(const LocklessRingBuffer *ring_ptr)
{
    return LocklessRingBufferSize(ring_ptr) == 0;
}

bool
LocklessRingBufferFull(const LocklessRingBuffer *ring_ptr)
{
    if (!ring_ptr) return false;
    return LocklessRingBufferSize(ring_ptr) >= ring_ptr->usable;
}
