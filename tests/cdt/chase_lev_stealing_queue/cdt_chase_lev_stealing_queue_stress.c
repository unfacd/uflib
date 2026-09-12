/**
 * @file cdt_chase_lev_stealing_queue_stress.c
 * @brief Standalone multi-threaded stress test for the Chase–Lev work-stealing
 *        deque — steal-only (producer + thieves) exactly-once.
 *
 * Starts at capacity 2 so the ring doubles many times while 8 thieves hold
 * old arrays; verifies every item is delivered exactly once via a seen[]
 * bitmap.  Usage: ./cdt_chase_lev_stealing_queue_stress
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

#define _GNU_SOURCE

#include <uflib/cdt/chase_lev_stealing_queue/cdt_chase_lev_stealing_queue.h>

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define THIEVES 8u
#define ITEMS 500000u
#define INITIAL_CAPACITY 2u

typedef struct {
    ChaseLevStealingQueue *q;
    _Atomic size_t produced;
    _Atomic size_t consumed;
    _Atomic unsigned done;
    _Atomic unsigned *seen;
    _Atomic unsigned bad;
} ctx_t;

static void *
owner(void *arg)
{
    ctx_t *c = arg;
    for (size_t i = 0u; i < ITEMS; ++i) {
        void *p = (void *)(uintptr_t)(i + 1u);
        while (!ChaseLevStealingQueuePush(c->q, p)) {
            sched_yield();
        }
        atomic_fetch_add_explicit(&c->produced, 1u, memory_order_relaxed);
        if ((i & 1023u) == 0u) {
            sched_yield();
        }
    }
    atomic_store_explicit(&c->done, 1u, memory_order_release);
    return NULL;
}

static void *
thief(void *arg)
{
    ctx_t *c = arg;
    for (;;) {
        void *p = NULL;
        if (ChaseLevStealingQueueSteal(c->q, &p)) {
            const size_t id = (size_t)(uintptr_t)p;
            if (id == 0u || id > ITEMS) {
                atomic_fetch_add_explicit(&c->bad, 1u, memory_order_relaxed);
                continue;
            }
            const unsigned previous = atomic_fetch_add_explicit(
                &c->seen[id - 1u], 1u, memory_order_relaxed);
            if (previous != 0u) {
                atomic_fetch_add_explicit(&c->bad, 1u, memory_order_relaxed);
            }
            atomic_fetch_add_explicit(&c->consumed, 1u, memory_order_relaxed);
            continue;
        }

        if (atomic_load_explicit(&c->done, memory_order_acquire) != 0u &&
            atomic_load_explicit(&c->consumed, memory_order_relaxed) == ITEMS) {
            break;
        }
        sched_yield();
    }
    return NULL;
}

int
main(void)
{
    ctx_t c = {0};
    c.q = ChaseLevStealingQueueCreate(INITIAL_CAPACITY);
    c.seen = calloc(ITEMS, sizeof(*c.seen));
    if (c.q == NULL || c.seen == NULL) {
        fprintf(stderr, "allocation failure\n");
        ChaseLevStealingQueueDestroy(c.q);
        free(c.seen);
        return 2;
    }

    pthread_t owner_thread;
    pthread_t thieves[THIEVES];
    if (pthread_create(&owner_thread, NULL, owner, &c) != 0) {
        fprintf(stderr, "pthread_create owner failed\n");
        return 2;
    }
    for (unsigned i = 0u; i < THIEVES; ++i) {
        if (pthread_create(&thieves[i], NULL, thief, &c) != 0) {
            fprintf(stderr, "pthread_create thief failed\n");
            return 2;
        }
    }

    pthread_join(owner_thread, NULL);
    for (unsigned i = 0u; i < THIEVES; ++i) {
        pthread_join(thieves[i], NULL);
    }

    size_t missing = 0u;
    for (size_t i = 0u; i < ITEMS; ++i) {
        if (atomic_load_explicit(&c.seen[i], memory_order_relaxed) != 1u) {
            ++missing;
        }
    }

    const size_t produced = atomic_load_explicit(&c.produced, memory_order_relaxed);
    const size_t consumed = atomic_load_explicit(&c.consumed, memory_order_relaxed);
    const unsigned bad = atomic_load_explicit(&c.bad, memory_order_relaxed);
    printf("initial_capacity=%u final_capacity=%zu produced=%zu consumed=%zu missing=%zu bad=%u\n",
           INITIAL_CAPACITY, ChaseLevStealingQueueCapacity(c.q), produced, consumed, missing, bad);

    free(c.seen);
    ChaseLevStealingQueueDestroy(c.q);
    return (produced == ITEMS && consumed == ITEMS && missing == 0u && bad == 0u) ? 0 : 1;
}
