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
 * Performance harness for the V1 configurable hopscotch store.
 *
 * Reports nanoseconds per operation for insert, hit-miss lookup, remove and a
 * steady-state hit lookup, at a range of table sizes.  The numbers are meant to
 * be recorded and compared across changes; the shape (one line per size, fixed
 * field order) is what the design doc's baseline table quotes, so keep it stable.
 *
 *   hopscotch_configurable_perf [--items N] [--repeat N] [--seed N]
 *
 * Build it optimised, and with the sanitizers off, or the numbers are
 * meaningless: they measure the harness as much as the table.
 */

#include <uflib/adt/adt_hopscotch_hashtable.h>

#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ITEMS  1000000
#define DEFAULT_REPEAT 3

static volatile uintptr_t sSink;

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

static uint64_t
sNowNs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void
sMeasure(size_t items, uint64_t seed, double *insert_ns, double *hit_ns, double *miss_ns, double *remove_ns,
         size_t *pfactor_out)
{
    HopscotchHashtableConfigurable store;
    uint64_t rng = seed | 1;
    uint64_t start;
    double insert, hit, miss, remove;

    memset(&store, 0, sizeof(store));
    if (hopscotch_init_with_offset(&store.hashtable, 4) == NULL) {
        fprintf(stderr, "perf: could not allocate the store\n");
        exit(2);
    }
    store.hash_func = sSpreadHash;

    start = sNowNs();
    for (size_t i = 0; i < items; i++) {
        if (hopscotch_insert_configurable(&store, (uint8_t *)sItem(i)) != 0) {
            fprintf(stderr, "perf: insert %zu failed\n", i);
            exit(2);
        }
    }
    insert = (double)(sNowNs() - start) / (double)items;

    start = sNowNs();
    for (size_t i = 0; i < items; i++) {
        sSink = (uintptr_t)hopscotch_lookup_configurable(&store, (uint8_t *)sItem(i));
    }
    hit = (double)(sNowNs() - start) / (double)items;

    start = sNowNs();
    for (size_t i = 0; i < items; i++) {
        sSink = (uintptr_t)hopscotch_lookup_configurable(&store, (uint8_t *)sItem(items + i));
    }
    miss = (double)(sNowNs() - start) / (double)items;

    if (pfactor_out != NULL) *pfactor_out = store.hashtable.pfactor;

    /* Remove in a shuffled order: worst case for the hopinfo chains, and it
     * catches a removal that only works for the most recently inserted key. */
    start = sNowNs();
    for (size_t i = 0; i < items; i++) {
        const size_t idx = (size_t)(sNext(&rng) % items);

        sSink = (uintptr_t)hopscotch_remove_configurable(&store, (uint8_t *)sItem(idx));
    }
    remove = (double)(sNowNs() - start) / (double)items;

    hopscotch_release(&store.hashtable);

    *insert_ns = insert;
    *hit_ns    = hit;
    *miss_ns   = miss;
    *remove_ns = remove;
}

int
main(int argc, char **argv)
{
    size_t items = DEFAULT_ITEMS;
    size_t repeat = DEFAULT_REPEAT;
    uint64_t seed = 0xa11ce;
    static struct option long_opts[] = {
        {"items",  required_argument, 0, 'i'},
        {"repeat", required_argument, 0, 'r'},
        {"seed",   required_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:r:s:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i': items  = (size_t)strtoull(optarg, NULL, 10); break;
        case 'r': repeat = (size_t)strtoull(optarg, NULL, 10); break;
        case 's': seed   = strtoull(optarg, NULL, 10); break;
        default:
            fprintf(stderr, "Usage: %s [--items N] [--repeat N] [--seed N]\n", argv[0]);
            return 1;
        }
    }

    if (items == 0 || repeat == 0) {
        fprintf(stderr, "perf: items and repeat must be non-zero\n");
        return 1;
    }

    printf("# configurable hopscotch store, ns/op, %zu items, %zu repeats, seed %" PRIu64 "\n", items, repeat, seed);
    printf("size,pfactor,insert_ns,lookup_hit_ns,lookup_miss_ns,remove_ns\n");

    for (size_t size = 1024; size <= items; size *= 8) {
        double best_insert = 0, best_hit = 0, best_miss = 0, best_remove = 0;
        size_t pfactor = 0;

        for (size_t r = 0; r < repeat; r++) {
            double insert, hit, miss, remove;

            sMeasure(size, seed + r, &insert, &hit, &miss, &remove, &pfactor);

            /* best-of-N: the least noise-contaminated sample */
            if (r == 0 || insert < best_insert) best_insert = insert;
            if (r == 0 || hit < best_hit) best_hit = hit;
            if (r == 0 || miss < best_miss) best_miss = miss;
            if (r == 0 || remove < best_remove) best_remove = remove;
        }

        printf("%zu,%zu,%.1f,%.1f,%.1f,%.1f\n", size, pfactor, best_insert, best_hit, best_miss, best_remove);
        fflush(stdout);
    }

    return 0;
}
