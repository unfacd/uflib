/**
 * @file cdt_lockless_minheap_stress.c
 * @brief Standalone multi-threaded stress test for LocklessMinHeap.
 *
 * Three adversarial scenarios, each with a zero-error gate:
 *
 *   1. unique tickets  — P producers insert unique keys; C consumers extract.
 *      Verifies no lost insert, no double extract, and that size_approx is
 *      exactly zero after the quiescent drain.
 *   2. duplicate keys  — every producer inserts the same key, forcing many
 *      consumers to race on the mark/unlink CAS (the helping path).
 *   3. sustained churn — producers and consumers run for a fixed wall-clock
 *      duration with random keys, then a final drain must leave the queue
 *      empty with zero errors.
 *
 * Built but NOT registered with CTest (requires CLI args).
 *
 * Usage:
 *   cdt_lockless_minheap_stress --prod P --cons C --n N [--duration S]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <uflib/cdt/lockless_minheap/cdt_lockless_minheap.h>

/* ── Time ───────────────────────────────────────────────────────────────── */

static double
sNowS(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── Scenario 1: unique tickets ─────────────────────────────────────────── */

struct sProdArg {
    LocklessMinHeap *h;
    int              tid;
    int              n;       /* per-producer count */
    _Atomic int     *fail;
};

static void *
sProducer(void *arg)
{
    struct sProdArg *a = (struct sProdArg *)arg;
    for (int i = 0; i < a->n; ++i) {
        int64_t key = (int64_t)a->tid * a->n + i;
        if (LocklessMinHeapInsertI64(a->h, key, (void *)(intptr_t)key) !=
            LOCKLESS_MINHEAP_OK) {
            atomic_store(a->fail, 1);
        }
    }
    return NULL;
}

struct sConsArg {
    LocklessMinHeap *h;
    _Atomic int     *consumed;
    _Atomic int     *done_prod;
    int              total;
    unsigned char   *seen;      /* seen[ticket]++ — 2nd increment => double extract */
    pthread_mutex_t *mu;
    _Atomic int     *fail;
};

static void *
sConsumer(void *arg)
{
    struct sConsArg *a = (struct sConsArg *)arg;
    int64_t k;
    void   *v;
    for (;;) {
        if (LocklessMinHeapDelminI64(a->h, &k, &v)) {
            if (k < 0 || k >= a->total) {
                atomic_store(a->fail, 1);
                continue;
            }
            pthread_mutex_lock(a->mu);
            if (a->seen[k]++)
                atomic_store(a->fail, 1);   /* double extract */
            pthread_mutex_unlock(a->mu);
            atomic_fetch_add(a->consumed, 1);
        } else if (atomic_load(a->done_prod)) {
            break;   /* producers done and queue empty */
        } else {
            sched_yield();
        }
    }
    return NULL;
}

static int
sRunUnique(int nprod, int ncons, int per)
{
    const int total = nprod * per;
    LocklessMinHeap *h = LocklessMinHeapCreate();
    if (!h)
        return 1;

    unsigned char *seen = calloc((size_t)total, 1);
    _Atomic int fail = 0, consumed = 0, done_prod = 0;
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

    pthread_t      *th = calloc((size_t)(nprod + ncons), sizeof(*th));
    struct sProdArg *pa = calloc((size_t)nprod, sizeof(*pa));
    struct sConsArg ca = {
        .h = h, .consumed = &consumed, .done_prod = &done_prod,
        .total = total, .seen = seen, .mu = &mu, .fail = &fail
    };

    double t0 = sNowS();
    for (int i = 0; i < nprod; ++i) {
        pa[i] = (struct sProdArg){ h, i, per, &fail };
        pthread_create(&th[i], NULL, sProducer, &pa[i]);
    }
    for (int i = 0; i < ncons; ++i)
        pthread_create(&th[nprod + i], NULL, sConsumer, &ca);

    for (int i = 0; i < nprod; ++i)
        pthread_join(th[i], NULL);
    atomic_store(&done_prod, 1);
    for (int i = 0; i < ncons; ++i)
        pthread_join(th[nprod + i], NULL);

    /* Final drain — catches any straggler in the extraction window. */
    int64_t k;
    while (LocklessMinHeapDelminI64(h, &k, NULL)) {
        if (k < 0 || k >= total) {
            atomic_store(&fail, 1);
            continue;
        }
        pthread_mutex_lock(&mu);
        if (seen[k]++)
            atomic_store(&fail, 1);
        pthread_mutex_unlock(&mu);
        atomic_fetch_add(&consumed, 1);
    }
    double t1 = sNowS();

    size_t leftover = LocklessMinHeapSizeApprox(h);
    int missing = 0;
    for (int i = 0; i < total; ++i)
        if (seen[i] != 1)
            missing++;

    int bad = atomic_load(&fail) || missing ||
              atomic_load(&consumed) != total || leftover != 0;

    printf("[lf-stress:unique] P=%d C=%d n/P=%d consumed=%d missing_or_dup=%d "
           "leftover=%zu  %.3fs  %s\n",
           nprod, ncons, per, atomic_load(&consumed), missing, leftover,
           t1 - t0, bad ? "FAIL" : "ok");

    LocklessMinHeapReclaim(h);
    LocklessMinHeapDestroy(h);
    free(seen);
    free(th);
    free(pa);
    pthread_mutex_destroy(&mu);
    return bad;
}

/* ── Scenario 2: duplicate keys (helping-path hammer) ───────────────────── */

static void *
sDupProducer(void *arg)
{
    struct sProdArg *a = (struct sProdArg *)arg;
    for (int i = 0; i < a->n; ++i) {
        if (LocklessMinHeapInsertI64(a->h, 7, NULL) != LOCKLESS_MINHEAP_OK)
            atomic_store(a->fail, 1);
    }
    return NULL;
}

static void *
sDupConsumer(void *arg)
{
    struct sConsArg *a = (struct sConsArg *)arg;
    for (;;) {
        if (LocklessMinHeapDelminI64(a->h, NULL, NULL)) {
            atomic_fetch_add(a->consumed, 1);
        } else if (atomic_load(a->done_prod)) {
            break;
        } else {
            sched_yield();
        }
    }
    return NULL;
}

static int
sRunDupes(int nprod, int ncons, int per)
{
    const int total = nprod * per;
    LocklessMinHeap *h = LocklessMinHeapCreate();
    if (!h)
        return 1;

    _Atomic int fail = 0, consumed = 0, done_prod = 0;
    pthread_t *th = calloc((size_t)(nprod + ncons), sizeof(*th));
    struct sProdArg *pa = calloc((size_t)nprod, sizeof(*pa));
    struct sConsArg ca = {
        .h = h, .consumed = &consumed, .done_prod = &done_prod,
        .total = total, .seen = NULL, .mu = NULL, .fail = &fail
    };

    double t0 = sNowS();
    for (int i = 0; i < nprod; ++i) {
        pa[i] = (struct sProdArg){ h, i, per, &fail };
        pthread_create(&th[i], NULL, sDupProducer, &pa[i]);
    }
    for (int i = 0; i < ncons; ++i)
        pthread_create(&th[nprod + i], NULL, sDupConsumer, &ca);

    for (int i = 0; i < nprod; ++i)
        pthread_join(th[i], NULL);
    atomic_store(&done_prod, 1);
    for (int i = 0; i < ncons; ++i)
        pthread_join(th[nprod + i], NULL);

    while (LocklessMinHeapDelminI64(h, NULL, NULL))
        atomic_fetch_add(&consumed, 1);
    double t1 = sNowS();

    size_t leftover = LocklessMinHeapSizeApprox(h);
    int bad = atomic_load(&fail) || atomic_load(&consumed) != total || leftover != 0;

    printf("[lf-stress:dupes ] P=%d C=%d n/P=%d consumed=%d leftover=%zu  "
           "%.3fs  %s\n",
           nprod, ncons, per, atomic_load(&consumed), leftover,
           t1 - t0, bad ? "FAIL" : "ok");

    LocklessMinHeapReclaim(h);
    LocklessMinHeapDestroy(h);
    free(th);
    free(pa);
    return bad;
}

/* ── Scenario 3: sustained churn ────────────────────────────────────────── */

struct sChurnCtx {
    LocklessMinHeap *h;
    _Atomic int     *fail;
    _Atomic int     *running;
    unsigned         seed;
};

static void *
sChurnWorker(void *arg)
{
    struct sChurnCtx *a = (struct sChurnCtx *)arg;

    while (atomic_load(a->running)) {
        /* Mix insert and delmin; keys are small so duplicates collide. */
        int64_t key = (int64_t)(rand_r(&a->seed) % 64) - 32;
        if ((rand_r(&a->seed) % 3) == 0) {
            (void)LocklessMinHeapDelminI64(a->h, NULL, NULL);
        } else {
            if (LocklessMinHeapInsertI64(a->h, key, NULL) != LOCKLESS_MINHEAP_OK)
                atomic_store(a->fail, 1);
        }
    }
    return NULL;
}

static int
sRunSustained(int nthreads, double seconds)
{
    LocklessMinHeap *h = LocklessMinHeapCreate();
    if (!h)
        return 1;

    _Atomic int fail = 0, running = 1;
    pthread_t        *th  = calloc((size_t)nthreads, sizeof(*th));
    struct sChurnCtx *ctx = calloc((size_t)nthreads, sizeof(*ctx));

    double t0 = sNowS();
    for (int i = 0; i < nthreads; ++i) {
        ctx[i].h       = h;
        ctx[i].fail    = &fail;
        ctx[i].running = &running;
        ctx[i].seed    = 0x9E3779B9u ^ (unsigned)(i * 2654435761u) ^ (unsigned)pthread_self();
        pthread_create(&th[i], NULL, sChurnWorker, &ctx[i]);
    }

    struct timespec ts = { (time_t)seconds, (long)((seconds - (int)seconds) * 1e9) };
    nanosleep(&ts, NULL);
    atomic_store(&running, 0);
    for (int i = 0; i < nthreads; ++i)
        pthread_join(th[i], NULL);
    double t1 = sNowS();

    /* Quiescent drain must empty the queue exactly. */
    size_t drained = 0;
    while (LocklessMinHeapDelminI64(h, NULL, NULL))
        drained++;

    size_t leftover = LocklessMinHeapSizeApprox(h);
    int bad = atomic_load(&fail) || leftover != 0;

    printf("[lf-stress:churn ] threads=%d duration=%.1fs drained=%zu leftover=%zu "
           "%.3fs  %s\n",
           nthreads, seconds, drained, leftover, t1 - t0, bad ? "FAIL" : "ok");

    LocklessMinHeapReclaim(h);
    LocklessMinHeapDestroy(h);
    free(th);
    free(ctx);
    return bad;
}

/* ── Usage / main ───────────────────────────────────────────────────────── */

static void
sPrintUsage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --prod P --cons C --n N [--duration S]\n"
        "  --prod P      producer threads (default 4)\n"
        "  --cons C      consumer threads (default 4)\n"
        "  --n N         per-producer ops (default 5000)\n"
        "  --duration S  sustained-churn seconds (default 3)\n",
        prog);
}

int
main(int argc, char **argv)
{
    int nprod = 4, ncons = 4, per = 5000;
    double duration = 3.0;

    static struct option long_opts[] = {
        {"prod",     required_argument, 0, 'p'},
        {"cons",     required_argument, 0, 'c'},
        {"n",        required_argument, 0, 'n'},
        {"duration", required_argument, 0, 'd'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:c:n:d:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': nprod    = atoi(optarg); break;
        case 'c': ncons    = atoi(optarg); break;
        case 'n': per      = atoi(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'h': sPrintUsage(argv[0]); return 0;
        default:  sPrintUsage(argv[0]); return 1;
        }
    }

    if (nprod < 1 || ncons < 1 || per < 1 || duration < 0.0) {
        fprintf(stderr, "error: prod/cons/n must be >= 1 and duration >= 0\n");
        return 1;
    }

    printf("=== LocklessMinHeap Multi-threaded Stress ===\n");
    printf("  producers=%d consumers=%d ops/producer=%d duration=%.1fs\n\n",
           nprod, ncons, per, duration);

    int rc = 0;
    rc |= sRunUnique(1, 1, 1000);
    rc |= sRunUnique(nprod, ncons, per);
    rc |= sRunUnique(8, 2, per / 2 + 1);
    rc |= sRunDupes(nprod, ncons, per);
    rc |= sRunSustained(nprod + ncons, duration);

    printf("\n%s\n", rc ? "RESULT: FAIL" : "RESULT: PASS");
    return rc;
}
