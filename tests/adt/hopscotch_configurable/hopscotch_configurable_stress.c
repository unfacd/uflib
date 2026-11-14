/**
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

/*
 * Standalone stress test for the V1 configurable hopscotch store.
 *
 * Every operation is checked against a model of what the store should contain,
 * so a lost or mis-bucketed item fails the run rather than silently changing a
 * later lookup.  Uniqueness is enforced by the driver, as the API requires.
 *
 *   hopscotch_configurable_stress [--ops N] [--pool N] [--pfactor N]
 *                                 [--seed N] [--threads N]
 *
 * A thread count above one runs independent stores in parallel, which is how
 * the store is used: one per thread, never shared.
 */

#include <uflib/adt/adt_hopscotch_hashtable.h>

#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_OPS     2000000
#define DEFAULT_POOL    8192
#define DEFAULT_PFACTOR 2
#define DEFAULT_THREADS 1

struct stress_config {
    size_t   ops;
    size_t   pool;
    size_t   pfactor;
    uint64_t seed;
};

struct stress_result {
    size_t insert, remove, lookup;
    size_t mismatch, lost;
    size_t pfactor_final;
};

struct run_state {
    struct stress_config cfg;
    struct stress_result result;
};

static uint64_t
sSpreadHash(const uint8_t *key, size_t key_len)
{
    uint64_t hash = 0xcbf29ce484222325ULL;

    for (size_t i = 0; i < key_len; i++) {
        hash ^= key[i];
        hash *= 0x100000001b3ULL;
    }

    return hash;
}

static uint64_t
sNext(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;

    return *state;
}

static void *
sItem(size_t idx)
{
    return (void *)(uintptr_t)(0x100000 + idx * 64);
}

static void
sFail(const char *what)
{
    fprintf(stderr, "stress: %s\n", what);
    exit(2);
}

static void *
sRun(void *arg)
{
    struct run_state *state = arg;
    const struct stress_config *cfg = &state->cfg;
    struct stress_result *res = &state->result;
    uint8_t *present = calloc(cfg->pool, sizeof(uint8_t));
    HopscotchHashtableConfigurable store;
    uint64_t rng = cfg->seed | 1;

    if (present == NULL) sFail("could not allocate the model set");

    memset(&store, 0, sizeof(store));
    if (hopscotch_init_with_offset(&store.hashtable, cfg->pfactor) == NULL) sFail("could not allocate the store");
    store.hash_func = sSpreadHash;

    /* Populate before the op loop so the measured operations run against a
     * populated, already-grown table. */
    for (size_t i = 0; i < cfg->pool; i++) {
        if (hopscotch_insert_configurable(&store, (uint8_t *)sItem(i)) != 0) {
            res->mismatch++;
            continue;
        }
        present[i] = 1;
        res->insert++;
    }

    for (size_t op = 0; op < cfg->ops; op++) {
        const size_t idx = (size_t)(sNext(&rng) % cfg->pool);
        void *ptr = sItem(idx);
        const unsigned action = (unsigned)(sNext(&rng) % 3);

        if (action == 0) {
            if (present[idx]) continue;
            if (hopscotch_insert_configurable(&store, (uint8_t *)ptr) != 0) res->mismatch++;
            present[idx] = 1;
            res->insert++;
        } else if (action == 1) {
            void *removed = hopscotch_remove_configurable(&store, (uint8_t *)ptr);

            if (present[idx]) {
                if (removed != ptr) res->mismatch++;
                present[idx] = 0;
            } else if (removed != NULL) {
                res->mismatch++;
            }
            res->remove++;
        } else {
            void *found = hopscotch_lookup_configurable(&store, (uint8_t *)ptr);

            if ((found != NULL) != (present[idx] != 0)) res->mismatch++;
            res->lookup++;
        }
    }

    for (size_t i = 0; i < cfg->pool; i++) {
        void *found = hopscotch_lookup_configurable(&store, (uint8_t *)sItem(i));

        if ((found != NULL) != (present[i] != 0)) res->lost++;
    }

    res->pfactor_final = store.hashtable.pfactor;

    hopscotch_release(&store.hashtable);
    free(present);

    return NULL;
}

int
main(int argc, char **argv)
{
    struct stress_config cfg = {
        .ops     = DEFAULT_OPS,
        .pool    = DEFAULT_POOL,
        .pfactor = DEFAULT_PFACTOR,
        .seed    = 0x5eed
    };
    size_t thread_count = DEFAULT_THREADS;
    static struct option long_opts[] = {
        {"ops",     required_argument, 0, 'o'},
        {"pool",    required_argument, 0, 'p'},
        {"pfactor", required_argument, 0, 'f'},
        {"seed",    required_argument, 0, 's'},
        {"threads", required_argument, 0, 't'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:p:f:s:t:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'o': cfg.ops     = (size_t)strtoull(optarg, NULL, 10); break;
        case 'p': cfg.pool    = (size_t)strtoull(optarg, NULL, 10); break;
        case 'f': cfg.pfactor = (size_t)strtoull(optarg, NULL, 10); break;
        case 's': cfg.seed    = strtoull(optarg, NULL, 10); break;
        case 't': thread_count = (size_t)strtoull(optarg, NULL, 10); break;
        default:
            fprintf(stderr, "Usage: %s [--ops N] [--pool N] [--pfactor N] [--seed N] [--threads N]\n", argv[0]);
            return 1;
        }
    }

    if (cfg.pool == 0 || thread_count == 0) {
        fprintf(stderr, "stress: pool and threads must be non-zero\n");
        return 1;
    }

    printf("Configurable hopscotch store stress\n");
    printf("  ops/thread: %zu\n  pool:       %zu\n  pfactor:    %zu\n  threads:    %zu\n  seed:       %" PRIu64 "\n",
           cfg.ops, cfg.pool, cfg.pfactor, thread_count, cfg.seed);

    pthread_t *threads = calloc(thread_count, sizeof(*threads));
    struct run_state *states = calloc(thread_count, sizeof(*states));
    if (threads == NULL || states == NULL) sFail("could not allocate run state");

    for (size_t t = 0; t < thread_count; t++) {
        states[t].cfg = cfg;
        states[t].cfg.seed = cfg.seed + t * 0x9e3779b97f4a7c15ULL;

        if (pthread_create(&threads[t], NULL, sRun, &states[t]) != 0) sFail("could not start a worker");
    }

    size_t insert = 0, remove = 0, lookup = 0, mismatch = 0, lost = 0, pfactor = 0;
    for (size_t t = 0; t < thread_count; t++) {
        pthread_join(threads[t], NULL);

        insert   += states[t].result.insert;
        remove   += states[t].result.remove;
        lookup   += states[t].result.lookup;
        mismatch += states[t].result.mismatch;
        lost     += states[t].result.lost;
        if (states[t].result.pfactor_final > pfactor) pfactor = states[t].result.pfactor_final;
    }

    printf("  inserts:  %zu\n  removes:  %zu\n  lookups:  %zu\n  pfactor:  %zu\n", insert, remove, lookup, pfactor);
    printf("  model mismatches: %zu\n  lost items:       %zu\n", mismatch, lost);

    free(threads);
    free(states);

    if (mismatch != 0 || lost != 0) {
        printf("RESULT: FAIL\n");

        return 1;
    }

    printf("RESULT: PASS\n");

    return 0;
}
