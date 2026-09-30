/**
 * @file cdt_chase_lev_stealing_queue.c
 * @brief Chase–Lev work-stealing deque — implementation.
 *
 * Single-owner, multi-thief Chase–Lev deque (Chase & Lev 2005; portable
 * C11/weak-memory treatment by Lê, Pop, Cohen, Zappa Nardelli 2013), with a
 * retain-until-destroy buffer-lifetime policy.  See the design doc
 * `UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_DESIGN.md` for the correctness argument.
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

#include <uflib/cdt/chase_lev_stealing_queue/cdt_chase_lev_stealing_queue.h>

#include "cdt_chase_lev_stealing_queue_priv.h"

#include <uflib/logger/logger.h>

#include <stdint.h>
#include <stdlib.h>

#define UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_MIN_CAPACITY 2u

static bool
is_pow2_size(size_t x)
{
    return x != 0u && (x & (x - 1u)) == 0u;
}

static bool
round_capacity(size_t requested, size_t *result)
{
    if (requested < UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_MIN_CAPACITY) {
        requested = UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_MIN_CAPACITY;
    }
    if (is_pow2_size(requested)) {
        *result = requested;
        return true;
    }

    size_t p = UFLIB_CDT_CHASE_LEV_STEALING_QUEUE_MIN_CAPACITY;
    while (p < requested) {
        if (p > SIZE_MAX / 2u) {
            return false;
        }
        p *= 2u;
    }
    *result = p;
    return true;
}

static ChaseLevStealingQueueArray *
array_alloc(size_t capacity)
{
    if (capacity > (SIZE_MAX - sizeof(ChaseLevStealingQueueArray)) /
                        sizeof(ChaseLevStealingQueueSlot)) {
        return NULL;
    }

    const size_t bytes =
        sizeof(ChaseLevStealingQueueArray) + capacity * sizeof(ChaseLevStealingQueueSlot);
    ChaseLevStealingQueueArray *a = malloc(bytes);
    if (a == NULL) {
        return NULL;
    }

    a->capacity = capacity;
    a->mask = capacity - 1u;
    a->older = NULL;
    for (size_t i = 0u; i < capacity; ++i) {
        atomic_init(&a->slots[i].value, NULL);
    }
    return a;
}

/*
 * Owner-only grow.  Old arrays are retained until Destroy.  Geometric growth
 * means the sum of retained capacities is strictly less than the current
 * capacity, so this is bounded, not a leak.
 */
static bool
grow(ChaseLevStealingQueue *q, uint64_t top, uint64_t bottom)
{
    ChaseLevStealingQueueArray *old =
        atomic_load_explicit(&q->array, memory_order_relaxed);
    if (old->capacity > SIZE_MAX / 2u) {
        return false;
    }

    const size_t new_capacity = old->capacity * 2u;
    ChaseLevStealingQueueArray *new_array = array_alloc(new_capacity);
    if (new_array == NULL) {
        return false;
    }

    for (uint64_t i = top; i < bottom; ++i) {
        void *value = atomic_load_explicit(
            &old->slots[(size_t)(i & (uint64_t)old->mask)].value,
            memory_order_relaxed);
        atomic_store_explicit(
            &new_array->slots[(size_t)(i & (uint64_t)new_array->mask)].value,
            value,
            memory_order_relaxed);
    }

    old->older = q->retired;
    q->retired = old;

    atomic_store_explicit(&q->array, new_array, memory_order_release);
    atomic_store_explicit(&q->capacity, new_capacity, memory_order_relaxed);
    return true;
}

ChaseLevStealingQueue *
ChaseLevStealingQueueCreateWithLogger(size_t requested_capacity, UfLogger *logger_ptr)
{
    size_t capacity = 0u;
    if (!round_capacity(requested_capacity, &capacity)) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "work-stealing deque rejected: requested capacity %zu is unrepresentable",
                            requested_capacity);
        }
        return NULL;
    }

    ChaseLevStealingQueue *q = calloc(1u, sizeof(*q));
    if (q == NULL) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr, "work-stealing deque handle allocation failed");
        }
        return NULL;
    }

    /* Set the borrow first, so every failure below can be reported through it. */
    q->uf_logger = logger_ptr;

    ChaseLevStealingQueueArray *initial = array_alloc(capacity);
    if (initial == NULL) {
        if (logger_ptr != NULL) {
            UF_LOGGER_ERROR(logger_ptr,
                            "work-stealing deque ring allocation failed: capacity=%zu", capacity);
        }
        free(q);
        return NULL;
    }

    atomic_init(&q->top, 0u);
    atomic_init(&q->bottom, 0u);
    atomic_init(&q->array, initial);
    atomic_init(&q->capacity, capacity);

    if (!atomic_is_lock_free(&q->top) ||
        !atomic_is_lock_free(&q->bottom) ||
        !atomic_is_lock_free(&q->array)) {
        /* Not a resource shortage: the lock-free guarantee this structure is
           built on does not hold on this platform, so no deque can work here.
           Reported as CRITICAL because retrying or reallocating cannot help. */
        if (logger_ptr != NULL) {
            UF_LOGGER_CRITICAL(logger_ptr,
                               "work-stealing deque unusable: 64-bit and pointer atomics are not lock-free on this platform");
        }
        free(initial);
        free(q);
        return NULL;
    }
    q->retired = NULL;
    q->cached_top = 0u;

    if (logger_ptr != NULL) {
        UF_LOGGER_DEBUG(logger_ptr, "work-stealing deque created: capacity=%zu", capacity);
    }
    return q;
}

ChaseLevStealingQueue *
ChaseLevStealingQueueCreate(size_t requested_capacity)
{
    return ChaseLevStealingQueueCreateWithLogger(requested_capacity, NULL);
}

void
ChaseLevStealingQueueDestroy(ChaseLevStealingQueue *q)
{
    UfLogger *logger_ptr;

    if (q == NULL) {
        return;
    }

    /* Read the borrow before anything is released: it lives in the handle that
       is about to be freed. */
    logger_ptr = q->uf_logger;

    ChaseLevStealingQueueArray *a = atomic_load_explicit(&q->array, memory_order_relaxed);
    free(a);

    a = q->retired;
    while (a != NULL) {
        ChaseLevStealingQueueArray *older = a->older;
        free(a);
        a = older;
    }

    if (logger_ptr != NULL) {
        UF_LOGGER_DEBUG(logger_ptr, "work-stealing deque destroyed");
    }
    free(q);
}

bool
ChaseLevStealingQueuePush(ChaseLevStealingQueue *q, void *item)
{
    if (q == NULL || item == NULL) {
        return false;
    }

    ChaseLevStealingQueueArray *a = atomic_load_explicit(&q->array, memory_order_relaxed);
    const uint64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);

    uint64_t t = q->cached_top;
    if (b - t >= (uint64_t)a->capacity) {
        t = atomic_load_explicit(&q->top, memory_order_acquire);
        q->cached_top = t;
        if (b - t >= (uint64_t)a->capacity) {
            if (!grow(q, t, b)) {
                return false;
            }
            a = atomic_load_explicit(&q->array, memory_order_relaxed);
        }
    }

    atomic_store_explicit(
        &a->slots[(size_t)(b & (uint64_t)a->mask)].value,
        item,
        memory_order_relaxed);

    atomic_store_explicit(&q->bottom, b + 1u, memory_order_release);
    return true;
}

bool
ChaseLevStealingQueuePop(ChaseLevStealingQueue *q, void **item)
{
    if (q == NULL || item == NULL) {
        return false;
    }

    const uint64_t b0 = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    if (b0 == 0u) {
        return false;
    }

    const uint64_t b = b0 - 1u;
    atomic_store_explicit(&q->bottom, b, memory_order_seq_cst);
    const uint64_t t = atomic_load_explicit(&q->top, memory_order_seq_cst);

    if (t <= b) {
        ChaseLevStealingQueueArray *a =
            atomic_load_explicit(&q->array, memory_order_relaxed);
        void *value = atomic_load_explicit(
            &a->slots[(size_t)(b & (uint64_t)a->mask)].value,
            memory_order_relaxed);

        if (t == b) {
            uint64_t expected = t;
            if (!atomic_compare_exchange_strong_explicit(
                    &q->top, &expected, t + 1u,
                    memory_order_seq_cst, memory_order_seq_cst)) {
                atomic_store_explicit(&q->bottom, b + 1u, memory_order_relaxed);
                return false;
            }
            atomic_store_explicit(&q->bottom, b + 1u, memory_order_relaxed);
            q->cached_top = t + 1u;
        }

        *item = value;
        return true;
    }

    atomic_store_explicit(&q->bottom, b + 1u, memory_order_relaxed);
    return false;
}

bool
ChaseLevStealingQueueSteal(ChaseLevStealingQueue *q, void **item)
{
    if (q == NULL || item == NULL) {
        return false;
    }

    const uint64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    const uint64_t b = atomic_load_explicit(&q->bottom, memory_order_acquire);

    if (t >= b) {
        return false;
    }

    ChaseLevStealingQueueArray *a =
        atomic_load_explicit(&q->array, memory_order_acquire);
    void *value = atomic_load_explicit(
        &a->slots[(size_t)(t & (uint64_t)a->mask)].value,
        memory_order_relaxed);

    uint64_t expected = t;
    if (!atomic_compare_exchange_strong_explicit(
            &q->top, &expected, t + 1u,
            memory_order_seq_cst, memory_order_relaxed)) {
        return false;
    }

    *item = value;
    return true;
}

size_t
ChaseLevStealingQueueCapacity(const ChaseLevStealingQueue *q)
{
    if (q == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&q->capacity, memory_order_relaxed);
}

uint64_t
ChaseLevStealingQueueApproxSize(const ChaseLevStealingQueue *q)
{
    if (q == NULL) {
        return 0u;
    }
    const uint64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    const uint64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    return (b > t) ? (b - t) : 0u;
}
