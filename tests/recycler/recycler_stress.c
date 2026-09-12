/**
 * @file recycler_stress.c
 * @brief Standalone multi-threaded stress test for Recycler V1
 *
 * Usage: recycler_stress --threads N --duration S --pool-size P
 *
 * Sustained Get/Put cycles across multiple threads with zero-error assertion.
 * Built but NOT registered with CTest (requires CLI args).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/recycler/recycler.h>
#include "recycler_priv.h"

typedef struct {
	int id;
} StressPayload;

static atomic_bool sRunning = true;
static atomic_ulong sTotalOps = 0;
static atomic_ulong sGetErrors = 0;
static atomic_ulong sPutErrors = 0;
static atomic_ulong sNullGets = 0;

static int sStressInitCallback(ClientContextData *data_ptr, size_t oid)
{
	StressPayload *p = (StressPayload *)data_ptr;
	p->id = (int)oid;
	return 0;
}

static int sStressInitGetCallback(InstanceHolder *ih, ContextData *ctx, size_t oid, unsigned long flags)
{
	(void)ih; (void)ctx; (void)oid; (void)flags;
	return 0;
}

static int sStressInitPutCallback(InstanceHolder *ih, ContextData *ctx, unsigned long flags)
{
	(void)ih; (void)ctx; (void)flags;
	return 0;
}

static char *sStressPrintCallback(InstanceHolder *ih, ContextData *ctx, unsigned long flags)
{
	(void)ih; (void)ctx; (void)flags;
	return NULL;
}

static int sStressDestructCallback(InstanceHolder *ih, ContextData *ctx, unsigned long flags)
{
	(void)ih; (void)ctx; (void)flags;
	return 0;
}

static RecyclerPoolOps sStressOps = {
	sStressInitCallback,
	sStressInitGetCallback,
	sStressInitPutCallback,
	sStressPrintCallback,
	sStressDestructCallback,
	NULL
};

static void *
sWorkerThread(void *arg)
{
	unsigned type = *(unsigned *)arg;

	while (atomic_load(&sRunning)) {
		InstanceHolder *ih = RecyclerGet(type, NULL, 0);
		if (ih == NULL) {
			atomic_fetch_add(&sNullGets, 1);
			continue;
		}

		StressPayload *p = (StressPayload *)GetInstance(ih);
		if (p == NULL) {
			atomic_fetch_add(&sGetErrors, 1);
			continue;
		}

		/* Simulate brief work */
		for (volatile int i = 0; i < 10; i++) {}

		int rc = RecyclerPut(type, ih, NULL, 0);
		if (rc != 0) {
			atomic_fetch_add(&sPutErrors, 1);
		}

		atomic_fetch_add(&sTotalOps, 1);
	}

	return NULL;
}

static void
sPrintUsage(const char *prog)
{
	fprintf(stderr, "Usage: %s --threads N --duration S --pool-size P\n", prog);
}

int
main(int argc, char *argv[])
{
	int threads = 4;
	int duration = 5;
	int pool_size = 1024;

	static struct option long_opts[] = {
		{"threads",  required_argument, 0, 't'},
		{"duration", required_argument, 0, 'd'},
		{"pool-size", required_argument, 0, 'p'},
		{"help",     no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "t:d:p:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 't': threads = atoi(optarg); break;
		case 'd': duration = atoi(optarg); break;
		case 'p': pool_size = atoi(optarg); break;
		case 'h': sPrintUsage(argv[0]); return 0;
		default:  sPrintUsage(argv[0]); return 1;
		}
	}

	printf("=== Recycler V1 Stress Test ===\n");
	printf("  Threads:   %d\n", threads);
	printf("  Duration:  %d s\n", duration);
	printf("  Pool-size: %d\n", pool_size);
	printf("===============================\n\n");

	RecyclerPoolHandle *handle = RecyclerInitTypePool(
		"StressPayload", sizeof(StressPayload),
		16, (size_t)pool_size, &sStressOps);

	if (handle == NULL) {
		fprintf(stderr, "ERROR: Failed to initialize recycler pool\n");
		return 1;
	}

	printf("Running for %d seconds...\n", duration);

	pthread_t *thread_ids = (pthread_t *)calloc((size_t)threads, sizeof(pthread_t));

	for (int i = 0; i < threads; i++) {
		pthread_create(&thread_ids[i], NULL, sWorkerThread, &handle->type);
	}

	/* Sleep for the test duration */
	struct timespec ts = {duration, 0};
	nanosleep(&ts, NULL);

	atomic_store(&sRunning, false);

	for (int i = 0; i < threads; i++) {
		pthread_join(thread_ids[i], NULL);
	}

	free(thread_ids);

	unsigned long total = atomic_load(&sTotalOps);
	unsigned long get_errs = atomic_load(&sGetErrors);
	unsigned long put_errs = atomic_load(&sPutErrors);
	unsigned long null_gets = atomic_load(&sNullGets);

	printf("\n=== Results ===\n");
	printf("  Total ops:        %lu\n", total);
	printf("  Throughput:       %lu ops/s\n", total / (unsigned long)duration);
	printf("  Get errors:       %lu (payload validation)\n", get_errs);
	printf("  Put errors:       %lu (refcount/queue)\n", put_errs);
	printf("  NULL gets:        %lu (pool exhaustion)\n", null_gets);
	printf("===================\n");

	if (get_errs > 0 || put_errs > 0) {
		printf("FAIL: Non-zero error count detected.\n");
		return 1;
	}

	printf("Stress test complete — zero errors.\n");
	return 0;
}
