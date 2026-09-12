/**
 * @file
 * @brief Standalone stress test for HopscotchHashTable V2.
 *
 * Sequential workload: insert N unique keys, lookup all N, remove N/2,
 * verify survivors, verify removed absent.
 *
 * Usage:
 *   hopscotch_hashtable_v2_stress --iterations 100000 --capacity 16384
 *
 * Returns 0 on success (zero errors), 1 on any error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <getopt.h>

#include <uflib/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2.h>

typedef struct {
    uint64_t key;
    uint64_t value;
} StressItem;

int main(int argc, char **argv)
{
    size_t iterations = 100000;
    size_t capacity   = 16384;

    /* Parse CLI args */
    static struct option long_opts[] = {
        {"iterations", required_argument, 0, 'n'},
        {"capacity",   required_argument, 0, 'c'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:c:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n': iterations = (size_t)strtoull(optarg, NULL, 10); break;
        case 'c': capacity   = (size_t)strtoull(optarg, NULL, 10); break;
        default:
            fprintf(stderr, "Usage: %s [--iterations N] [--capacity C]\n", argv[0]);
            return 1;
        }
    }

    /* Compute pfactor from capacity (round up to power of 2) */
    size_t pfactor = 0;
    while ((1UL << pfactor) < capacity) pfactor++;

    printf("HopscotchHashTable V2 Stress Test\n");
    printf("  iterations: %zu\n", iterations);
    printf("  capacity:   %zu (pfactor=%zu, actual=%zu)\n",
           capacity, pfactor, 1UL << pfactor);

    /* ── Create ──────────────────────────────────────────────────── */
    HopscotchHashTableConfig cfg = {
        .pfactor = pfactor,
        .key_offset = offsetof(StressItem, key),
        .key_len = sizeof(uint64_t),
    };
    HopscotchHashTable *ht = HopscotchHashTableCreate(&cfg);
    if (!ht) {
        fprintf(stderr, "ERROR: HopscotchHashTableCreate returned NULL\n");
        return 1;
    }

    StressItem *items = calloc(iterations, sizeof(StressItem));
    if (!items) {
        fprintf(stderr, "ERROR: item allocation failed\n");
        HopscotchHashTableDestroy(ht);
        return 1;
    }

    size_t errors = 0;
    clock_t start, end;

    /* ── Insert phase ────────────────────────────────────────────── */
    start = clock();
    for (size_t i = 0; i < iterations; i++) {
        items[i].key = i + 1;
        items[i].value = i * 2;
        int rc = HopscotchHashTableInsert(ht, &items[i]);
        if (rc != 0) {
            fprintf(stderr, "ERROR: insert %zu returned %d\n", i, rc);
            errors++;
            if (errors > 10) { fprintf(stderr, "Too many errors, aborting.\n"); goto done; }
        }
    }
    end = clock();
    printf("  insert:  %zu ops in %.3fs (%.0f ops/sec)\n",
           iterations,
           (double)(end - start) / CLOCKS_PER_SEC,
           (double)iterations / ((double)(end - start) / CLOCKS_PER_SEC));

    if (HopscotchHashTableSize(ht) != iterations) {
        fprintf(stderr, "ERROR: size after insert = %zu, expected %zu\n",
                HopscotchHashTableSize(ht), iterations);
        errors++;
    }

    /* ── Lookup phase ────────────────────────────────────────────── */
    start = clock();
    for (size_t i = 0; i < iterations; i++) {
        uint64_t key = i + 1;
        StressItem *found = (StressItem *)HopscotchHashTableLookup(
            ht, (const uint8_t *)&key);
        if (!found) {
            fprintf(stderr, "ERROR: lookup %zu (key=%lu) returned NULL\n", i, key);
            errors++;
            if (errors > 10) { fprintf(stderr, "Too many errors, aborting.\n"); goto done; }
        } else if (found->key != key) {
            fprintf(stderr, "ERROR: lookup %zu returned wrong item (key=%lu, expected=%lu)\n",
                    i, found->key, key);
            errors++;
        } else if (found->value != i * 2) {
            fprintf(stderr, "ERROR: lookup %zu returned item with wrong value (%lu != %lu)\n",
                    i, found->value, i * 2);
            errors++;
        }
    }
    end = clock();
    printf("  lookup:  %zu ops in %.3fs (%.0f ops/sec)\n",
           iterations,
           (double)(end - start) / CLOCKS_PER_SEC,
           (double)iterations / ((double)(end - start) / CLOCKS_PER_SEC));

    /* ── Remove half ─────────────────────────────────────────────── */
    size_t remove_count = iterations / 2;
    start = clock();
    for (size_t i = 0; i < remove_count; i++) {
        uint64_t key = i + 1;
        StressItem *removed = (StressItem *)HopscotchHashTableRemove(
            ht, (const uint8_t *)&key);
        if (!removed) {
            fprintf(stderr, "ERROR: remove %zu (key=%lu) returned NULL\n", i, key);
            errors++;
            if (errors > 10) { fprintf(stderr, "Too many errors, aborting.\n"); goto done; }
        }
    }
    end = clock();
    printf("  remove:  %zu ops in %.3fs (%.0f ops/sec)\n",
           remove_count,
           (double)(end - start) / CLOCKS_PER_SEC,
           (double)remove_count / ((double)(end - start) / CLOCKS_PER_SEC));

    if (HopscotchHashTableSize(ht) != iterations - remove_count) {
        fprintf(stderr, "ERROR: size after remove = %zu, expected %zu\n",
                HopscotchHashTableSize(ht), iterations - remove_count);
        errors++;
    }

    /* ── Verify survivors ────────────────────────────────────────── */
    for (size_t i = remove_count; i < iterations; i++) {
        uint64_t key = i + 1;
        StressItem *found = (StressItem *)HopscotchHashTableLookup(
            ht, (const uint8_t *)&key);
        if (!found) {
            fprintf(stderr, "ERROR: survivor %zu (key=%lu) not found after removes\n", i, key);
            errors++;
            if (errors > 10) break;
        }
    }

    /* ── Verify removed items are gone ───────────────────────────── */
    for (size_t i = 0; i < remove_count; i++) {
        uint64_t key = i + 1;
        StressItem *found = (StressItem *)HopscotchHashTableLookup(
            ht, (const uint8_t *)&key);
        if (found) {
            fprintf(stderr, "ERROR: removed item %zu (key=%lu) still present\n", i, key);
            errors++;
            if (errors > 10) break;
        }
    }

done:
    free(items);
    HopscotchHashTableDestroy(ht);

    if (errors > 0) {
        printf("\nFAILED — %zu errors\n", errors);
        return 1;
    }

    printf("\nPASSED — zero errors\n");
    return 0;
}
