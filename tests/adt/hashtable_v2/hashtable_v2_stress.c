/**
 * @file hashtable_v2_stress.c
 * @brief Standalone multi-threaded stress test for HashTableV2.
 *
 * Usage:
 *   hashtable_v2_stress --threads 8 --duration 10 --capacity 65536
 *
 * Not registered with CTest — requires CLI arguments (build-only target).
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#include <uflib/adt/hashtable_v2/hashtable_v2.h>
#include <uflib/adt/hashtable_v2/hashtable_v2_type.h>
#include <uflib/adt/hashtable_v2/hashtable_v2_defs.h>

/* ── CLI defaults ─────────────────────────── */
#define DEFAULT_THREADS  4
#define DEFAULT_DURATION 5
#define DEFAULT_CAPACITY 65536

/* ── Global state ─────────────────────────── */
static volatile sig_atomic_t g_running = 1;
static HashTableV2         *g_ht     = NULL;

static struct {
	_Atomic unsigned long long inserts;
	_Atomic unsigned long long lookups;
	_Atomic unsigned long long removes;
	_Atomic unsigned long long insert_failures;
	_Atomic unsigned long long lookup_misses;
	_Atomic unsigned long long remove_misses;
} g_stats;

/* ── Signals ──────────────────────────────── */
static void
sSigHandler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ── Item type ────────────────────────────── */
typedef struct {
	uint64_t key;
	char     pad[56];  /* pad to cache-line size to avoid false sharing */
} StressItem;

static const void *
sStressKeyExtractor(HashTableV2Item *item_ptr)
{
	return &((StressItem *)item_ptr)->key;
}

/* ── Worker thread ────────────────────────── */
static void *
sWorker(void *arg)
{
	unsigned int seed = (unsigned int)(uintptr_t)arg;
	unsigned long long local_inserts  = 0;
	unsigned long long local_lookups  = 0;
	unsigned long long local_removes  = 0;
	unsigned long long local_ins_fail = 0;
	unsigned long long local_lkp_miss = 0;
	unsigned long long local_rem_miss = 0;

	while (g_running) {
		/* Generate a pseudo-random key biased toward reuse */
		uint64_t key = ((uint64_t)rand_r(&seed) << 32) | rand_r(&seed);

		int op = rand_r(&seed) % 10;
		if (op < 5) {
			/* Insert (50%) */
			StressItem *item = (StressItem *)calloc(1, sizeof(StressItem));
			if (!item) continue;
			item->key = key;

			HashTableV2Item *stored = HashTableV2Insert(g_ht, item, NULL);
			if (stored != item) {
				/* Already present — free our allocation */
				free(item);
				if (stored == NULL) {
					local_ins_fail++;
				}
			}
			local_inserts++;
		} else if (op < 9) {
			/* Lookup (40%) */
			HashTableV2Item *found = HashTableV2Lookup(g_ht, &key);
			if (found == NULL) local_lkp_miss++;
			local_lookups++;
		} else {
			/* Remove (10%) — need item pointer; skip if not found */
			StressItem dummy;
			dummy.key = key;
			HashTableV2Item *removed = HashTableV2Remove(g_ht, &dummy);
			if (removed != NULL) {
				free(removed);  /* we allocated it on insert */
			} else {
				local_rem_miss++;
			}
			local_removes++;
		}
	}

	/* Accumulate into global stats (relaxed — single writer per field after join) */
	atomic_fetch_add_explicit(&g_stats.inserts, local_inserts, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_stats.lookups, local_lookups, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_stats.removes, local_removes, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_stats.insert_failures, local_ins_fail, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_stats.lookup_misses, local_lkp_miss, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_stats.remove_misses, local_rem_miss, memory_order_relaxed);

	return NULL;
}

/* ── Main ─────────────────────────────────── */
int
main(int argc, char *argv[])
{
	int           threads       = DEFAULT_THREADS;
	int           duration      = DEFAULT_DURATION;
	unsigned long capacity_hint = DEFAULT_CAPACITY;

	/* Parse CLI */
	static struct option long_opts[] = {
		{"threads",  required_argument, NULL, 't'},
		{"duration", required_argument, NULL, 'd'},
		{"capacity", required_argument, NULL, 'c'},
		{"help",     no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "t:d:c:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 't': threads  = atoi(optarg); break;
		case 'd': duration = atoi(optarg); break;
		case 'c': capacity_hint = (unsigned long)atol(optarg); break;
		case 'h':
		default:
			fprintf(stderr,
			        "Usage: %s [--threads N] [--duration S] [--capacity C]\n"
			        "  --threads  N   Number of worker threads (default %d)\n"
			        "  --duration S   Run duration in seconds (default %d)\n"
			        "  --capacity C   Initial slot capacity hint (default %lu)\n",
			        argv[0], DEFAULT_THREADS, DEFAULT_DURATION,
			        (unsigned long)DEFAULT_CAPACITY);
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (threads < 1) threads = 1;
	if (duration < 1) duration = 1;

	/* Banner */
	printf("\n=== HashTableV2 Stress Test ===\n");
	printf("  Threads:    %d\n", threads);
	printf("  Duration:   %d s\n", duration);
	printf("  Capacity:   %lu slots\n", capacity_hint);
	printf("=============================================\n\n");

	/* Create table */
	HashTableV2Config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.key_size       = sizeof(uint64_t);
	cfg.key_offset     = 0;  /* key is first field of StressItem */
	cfg.key_extractor  = sStressKeyExtractor;
	cfg.enable_locking = true;
	cfg.capacity_hint  = capacity_hint;
	cfg.name           = "StressTest";

	g_ht = HashTableV2Create(&cfg);
	if (!g_ht) {
		fprintf(stderr, "FATAL: HashTableV2Create failed\n");
		return 1;
	}

	/* Install signal handler */
	signal(SIGINT, sSigHandler);
	signal(SIGALRM, sSigHandler);

	/* Spawn threads */
	pthread_t *th = (pthread_t *)calloc((size_t)threads, sizeof(pthread_t));
	if (!th) {
		fprintf(stderr, "FATAL: thread array allocation failed\n");
		HashTableV2Destroy(g_ht);
		return 1;
	}

	for (int i = 0; i < threads; i++) {
		if (pthread_create(&th[i], NULL, sWorker,
		                   (void *)(uintptr_t)(i + 1)) != 0) {
			fprintf(stderr, "FATAL: pthread_create failed for thread %d\n", i);
			g_running = 0;
			for (int j = 0; j < i; j++) pthread_join(th[j], NULL);
			free(th);
			HashTableV2Destroy(g_ht);
			return 1;
		}
	}

	/* Run for the specified duration */
	printf("Running for %d seconds...\n", duration);
	alarm((unsigned int)duration);
	for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
	free(th);

	/* Results */
	unsigned long long total_inserts = atomic_load(&g_stats.inserts);
	unsigned long long total_lookups = atomic_load(&g_stats.lookups);
	unsigned long long total_removes = atomic_load(&g_stats.removes);
	unsigned long long ins_fail = atomic_load(&g_stats.insert_failures);
	unsigned long long lkp_miss = atomic_load(&g_stats.lookup_misses);
	unsigned long long rem_miss = atomic_load(&g_stats.remove_misses);
	unsigned long long total_ops = total_inserts + total_lookups + total_removes;
	unsigned long long ops_per_sec = total_ops / (unsigned long long)duration;

	printf("\n=== Results ===\n");
	printf("  Total ops:        %llu\n", total_ops);
	printf("  Throughput:       %llu ops/s\n", ops_per_sec);
	printf("  Inserts:          %llu\n", total_inserts);
	printf("  Lookups:          %llu\n", total_lookups);
	printf("  Removes:          %llu\n", total_removes);
	printf("  Insert failures:  %llu (table full)\n", ins_fail);
	printf("  Lookup misses:    %llu (key absent)\n", lkp_miss);
	printf("  Remove misses:    %llu (key absent)\n", rem_miss);
	printf("  Final size:       %zu\n", HashTableV2Size(g_ht));
	printf("===================\n");

	/* Enumerate and free remaining caller-owned items */
	{
		size_t remaining = HashTableV2Size(g_ht);
		if (remaining > 0) {
			HashTableV2Item **items = (HashTableV2Item **)calloc(remaining, sizeof(HashTableV2Item *));
			if (items) {
				long n = HashTableV2Enumerate(g_ht, items, (long)remaining);
				for (long i = 0; i < n; i++) free(items[i]);
				free(items);
			}
		}
	}

	/* Clean up */
	HashTableV2Destroy(g_ht);

	printf("Stress test complete.\n");
	return 0;
}
