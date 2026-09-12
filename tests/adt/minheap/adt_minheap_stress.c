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

// adt_minheap_stress.c
// Standalone multithreaded stress for the single-threaded binary min-heap.
// The heap itself is single-owner; concurrency here exercises the two
// supported usage shapes:
//   1. N threads each owning a *private* heap (allocator / isolation stress),
//   2. one *shared* heap serialized by a pthread mutex (--mutex).
// Also runs sequential pointer-key and i64 order checks and a wide-comparator
// regression probe.  Exit 0 on all-pass, non-zero otherwise.

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <uflib/adt/adt_minheap.h>

static int cmp_int(const void *a, const void *b)
{
    int va = *(const int *)a, vb = *(const int *)b;
    return (va > vb) - (va < vb);
}

static int cmp_wide(const void *a, const void *b)
{
    int va = *(const int *)a, vb = *(const int *)b;
    if (va < vb) return -2;
    if (va > vb) return 2;
    return 0;
}

static void die(const char *msg) { perror(msg); exit(2); }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int seq_ptr(int n, int initial)
{
    MinHeap *h = MinHeapCreate(initial, cmp_int);
    if (!h) return 1;
    int *keys = malloc((size_t)n * sizeof(int));
    if (!keys) die("malloc");
    for (int i = 0; i < n; i++)
        keys[i] = (int)((0x9E3779B9u * (unsigned)(i + 1)) & 0x7fffffff);
    double t0 = now_s();
    if (MinHeapReserve(h, n) != MINHEAP_OK) return 1;
    for (int i = 0; i < n; i++)
        if (MinHeapInsert(h, &keys[i], &keys[i]) != MINHEAP_OK) return 1;
    if (MinHeapSize(h) != n) return 1;
    int prev = -1, first = 1, extracted = 0;
    void *k;
    while (MinHeapDelmin(h, &k, NULL)) {
        int cur = *(int *)k;
        if (!first && cur < prev) { fprintf(stderr, "order violated\n"); return 1; }
        first = 0;
        prev = cur;
        extracted++;
    }
    double t1 = now_s();
    MinHeapClear(h);
    if (MinHeapSize(h) != 0) return 1;
    MinHeapDestroy(h);
    free(keys);
    printf("[seq-ptr] n=%d initial=%d  %.3fs  ok\n", n, initial, t1 - t0);
    return extracted != n;
}

static int seq_i64(int n)
{
    MinHeap *h = MinHeapCreateI64(1);
    if (!h) return 1;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        int64_t k = (int64_t)((0x9E3779B97F4A7C15ull * (unsigned)(i + 3)) >> 33);
        if (MinHeapInsertI64(h, k, NULL) != MINHEAP_OK) return 1;
    }
    int64_t prev = INT64_MIN, k;
    int extracted = 0;
    while (MinHeapDelminI64(h, &k, NULL)) {
        if (k < prev) { fprintf(stderr, "i64 order violated\n"); return 1; }
        prev = k;
        extracted++;
    }
    double t1 = now_s();
    MinHeapDestroy(h);
    printf("[seq-i64] n=%d  %.3fs  ok\n", n, t1 - t0);
    return extracted != n;
}

static int wide_cmp(void)
{
    MinHeap *h = MinHeapCreate(8, cmp_wide);
    int keys[] = {9, 1, 5, 3, 7};
    const int expect[] = {1, 3, 5, 7, 9};
    for (int i = 0; i < 5; i++)
        MinHeapInsert(h, &keys[i], &keys[i]);
    int rc = 0;
    for (int i = 0; i < 5; i++) {
        void *k;
        if (!MinHeapDelmin(h, &k, NULL) || *(int *)k != expect[i]) {
            fprintf(stderr, "[wide-cmp] FAIL at %d\n", i);
            rc = 1;
            break;
        }
    }
    MinHeapDestroy(h);
    if (!rc) printf("[wide-cmp] ok (any-magnitude comparator)\n");
    return rc;
}

static int reserve_overflow(void)
{
    MinHeap *h = MinHeapCreate(4, cmp_int);
    int rc = 0;
    if (MinHeapReserve(h, 2000000000) != MINHEAP_ERR_OOM) {
        fprintf(stderr, "[reserve-overflow] FAIL: expected MINHEAP_ERR_OOM\n");
        rc = 1;
    }
    int k = 1;
    if (MinHeapInsert(h, &k, &k) != MINHEAP_OK || MinHeapSize(h) != 1) {
        fprintf(stderr, "[reserve-overflow] FAIL: heap unusable after reject\n");
        rc = 1;
    }
    MinHeapDestroy(h);
    if (!rc) printf("[reserve-overflow] ok (sGrowTo guard)\n");
    return rc;
}

struct worker_args { int ops, initial; unsigned seed; _Atomic int *fail; };

static void *private_worker(void *arg)
{
    struct worker_args *a = arg;
    MinHeap *h = MinHeapCreate(a->initial, cmp_int);
    if (!h) { atomic_store(a->fail, 1); return NULL; }
    enum { CAP = 4096 };
    int store[CAP];
    int nstore = 0;
    unsigned s = a->seed;
    for (int i = 0; i < a->ops; i++) {
        s = s * 1103515245u + 12345u;
        int live = MinHeapSize(h);
        if (live == 0)
            nstore = 0;
        int do_ins = (nstore < CAP) && (live == 0 || ((s >> 16) % 3) != 0);
        if (do_ins) {
            store[nstore] = (int)((s >> 8) & 0xffff);
            if (MinHeapInsert(h, &store[nstore], &store[nstore]) != MINHEAP_OK) {
                atomic_store(a->fail, 1);
                break;
            }
            nstore++;
        } else if (!MinHeapPop(h, NULL)) {
            atomic_store(a->fail, 1);
            break;
        }
    }
    void *k;
    int prev = -1, first = 1;
    while (MinHeapDelmin(h, &k, NULL)) {
        int cur = *(int *)k;
        if (!first && cur < prev) { atomic_store(a->fail, 1); break; }
        first = 0;
        prev = cur;
    }
    MinHeapDestroy(h);
    return NULL;
}

static int private_mt(int threads, int ops, int initial)
{
    pthread_t *th = calloc((size_t)threads, sizeof(*th));
    struct worker_args *args = calloc((size_t)threads, sizeof(*args));
    _Atomic int fail = 0;
    if (!th || !args) die("calloc");
    double t0 = now_s();
    for (int i = 0; i < threads; i++) {
        args[i].ops = ops;
        args[i].initial = initial;
        args[i].seed = 0xC0FFEEu ^ (unsigned)(i * 7919);
        args[i].fail = &fail;
        if (pthread_create(&th[i], NULL, private_worker, &args[i])) die("pthread_create");
    }
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    printf("[private-mt] threads=%d ops/thread=%d  %.3fs  %s\n",
           threads, ops, now_s() - t0, fail ? "FAIL" : "ok");
    free(th);
    free(args);
    return fail ? 1 : 0;
}

struct shared {
    MinHeap *h;
    pthread_mutex_t mu;
    int *store;
    int cap;
    _Atomic int next;
    _Atomic int fail;
    int ops_per_thread;
};

static void *shared_worker(void *arg)
{
    struct shared *S = arg;
    for (int i = 0; i < S->ops_per_thread; i++) {
        unsigned r = (unsigned)i * 2654435761u + (unsigned)(uintptr_t)pthread_self();
        pthread_mutex_lock(&S->mu);
        int sz = MinHeapSize(S->h);
        if (sz == 0 || (r & 3) != 0) {
            int idx = atomic_fetch_add(&S->next, 1);
            if (idx < S->cap) {
                S->store[idx] = (int)(r & 0x7fffffff);
                if (MinHeapInsert(S->h, &S->store[idx], &S->store[idx]) != MINHEAP_OK)
                    atomic_store(&S->fail, 1);
            } else if (sz > 0 && !MinHeapPop(S->h, NULL)) {
                atomic_store(&S->fail, 1);
            }
        } else if (!MinHeapPop(S->h, NULL)) {
            atomic_store(&S->fail, 1);
        }
        pthread_mutex_unlock(&S->mu);
    }
    return NULL;
}

static int shared_mutex_mt(int threads, int ops, int initial)
{
    struct shared S;
    memset(&S, 0, sizeof(S));
    S.h = MinHeapCreate(initial, cmp_int);
    pthread_mutex_init(&S.mu, NULL);
    S.cap = threads * ops + 8;
    S.store = calloc((size_t)S.cap, sizeof(int));
    if (!S.store) die("calloc");
    atomic_init(&S.next, 0);
    atomic_init(&S.fail, 0);
    S.ops_per_thread = ops;
    pthread_t *th = calloc((size_t)threads, sizeof(*th));
    double t0 = now_s();
    for (int i = 0; i < threads; i++)
        if (pthread_create(&th[i], NULL, shared_worker, &S)) die("pthread_create");
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    pthread_mutex_lock(&S.mu);
    void *k;
    int prev = -1, first = 1;
    while (MinHeapDelmin(S.h, &k, NULL)) {
        int cur = *(int *)k;
        if (!first && cur < prev) atomic_store(&S.fail, 1);
        first = 0;
        prev = cur;
    }
    pthread_mutex_unlock(&S.mu);
    printf("[shared-mutex] threads=%d ops/thread=%d  %.3fs  %s\n",
           threads, ops, now_s() - t0, atomic_load(&S.fail) ? "FAIL" : "ok");
    MinHeapDestroy(S.h);
    pthread_mutex_destroy(&S.mu);
    free(S.store);
    free(th);
    return atomic_load(&S.fail) ? 1 : 0;
}

int main(int argc, char **argv)
{
    int threads = 4, ops = 20000, n = 50000, initial = 64, use_mutex = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ops") && i + 1 < argc) ops = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--initial") && i + 1 < argc) initial = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mutex")) use_mutex = 1;
    }
    printf("adt_minheap stress  page=%d entries/page=%d\n",
           MinHeapPageSize(), MinHeapEntriesPerPage());
    int rc = 0;
    rc |= seq_ptr(n, initial);
    rc |= seq_ptr(n / 2 + 1, 1);
    rc |= seq_i64(n / 2);
    rc |= wide_cmp();
    rc |= reserve_overflow();
    rc |= private_mt(threads, ops, initial);
    if (use_mutex)
        rc |= shared_mutex_mt(threads, ops, initial);
    else
        printf("[shared-mutex] skipped (pass --mutex)\n");
    printf(rc ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return rc;
}
