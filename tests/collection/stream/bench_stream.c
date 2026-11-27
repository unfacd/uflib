/*
 * bench_stream.c — fixed-cost and per-element cost of the same pipeline through:
 *   - the fluent stream facade
 *   - the plain stream API
 *   - CollectionDescriptorV2 + CollectorRun
 *   - a hand-written C loop
 *
 * Build (Linux, for allocation counts):
 *   clang -std=gnu17 -O2 -fblocks -DBENCH_WRAP_ALLOC -Iinclude/uflib -Iinclude \
 *     src/collection/arena.c src/collection/collection.c \
 *     src/collection/stream/stream.c src/collection/stream/stream_fluent.c \
 *     tests/collection/stream/bench_stream.c -lBlocksRuntime -pthread \
 *     -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc \
 *     -o bench_stream
 *   taskset -c 2 ./bench_stream
 *
 * N=0 measures chain construction + terminal; N=10 measures the same pipeline
 * over ten ints. ns/elem is (N=10 - N=0) / 10, so fixed cost is removed.
 * Allocation counts are steady-state (after warm-up) per chain and cover calls
 * linked into the executable, not calls internal to libBlocksRuntime.so.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "collection/stream/stream_fluent.h"

#define N       10
#define ITERS   1000000
#define REPS    7
#define WARM    100000

static int  g_in[N];
static long g_out[N];
static volatile long g_sink;
static size_t g_len;

/* ---- allocation counters (wrapped malloc/calloc/realloc) ----
 * Built only on Linux with -Wl,--wrap=...; the counts therefore cover calls
 * linked into this executable (libuflib + this file), not calls made inside
 * libBlocksRuntime.so. */
#ifdef BENCH_WRAP_ALLOC
static unsigned long g_mallocs, g_callocs, g_reallocs;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t n)              { g_mallocs++;  return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t s)    { g_callocs++;  return __real_calloc(n, s); }
void *__wrap_realloc(void *p, size_t n)    { g_reallocs++; return __real_realloc(p, n); }

static void
alloc_reset(void)
{
  g_mallocs = g_callocs = g_reallocs = 0;
}

static unsigned long
alloc_total(void)
{
  return g_mallocs + g_callocs + g_reallocs;
}
#else
static void alloc_reset(void) {}
static unsigned long alloc_total(void) { return 0; }
#endif

static uint64_t
now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- pipelines ---- */
static void
hand_once(void)
{
  long   *dst = g_out;
  size_t  n   = 0;

  #pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
  for (size_t i = 0; i < g_len; i++) {
    int v = g_in[i];
    if (v > 3) dst[n++] = (long)v * 10;
  }
  g_sink += (n ? dst[n - 1] : 0) + (long)n;
}

static bool
d_next(const void *src, size_t *pos, const void **item, void *ctx)
{
  const int *base = src;
  (void)ctx;
  if (*pos >= g_len) return false;
  *item = &base[(*pos)++];
  return true;
}

static bool
d_validate(const void *src, size_t *hint, void *ctx)
{
  (void)src; (void)ctx;
  *hint = g_len;
  return true;
}

static CollectVerdict
d_process(const void *item, void *dst, Arena *arena, void *ctx)
{
  int v = *(const int *)item;
  (void)arena; (void)ctx;
  if (v <= 3) return COLLECT_SKIP;
  *(long *)dst = (long)v * 10;
  return COLLECT_TAKE;
}

static const CollectionDescriptorV2 g_desc = {
  .elem_sz      = sizeof(long),
  .on_validate  = d_validate,
  .on_next      = d_next,
  .on_process   = d_process,
  .allow_empty  = true,
};

static StreamCollectOpts g_opts = {
  .elem_sz     = sizeof(long),
  .allow_empty = true,
};

static void
desc_once(void)
{
  CollectionResult out;

  assert(CollectorRun(&g_desc, g_in, &out) == COLLECT_OK);
  g_sink += (out.count ? ((long *)out.items)[out.count - 1] : 0) + (long)out.count;
  CollectionResultFree(&g_desc, &out, NULL);
}

static void
plain_collect_once(void)
{
  CollectionResult out;
  Stream *s = StreamOfArray(g_in, g_len, sizeof(int));

  StreamFilter(s, ^bool(const void *p) { return *(const int *)p > 3; });
  StreamMap(s, sizeof(long), ^StreamVerdict(const void *in, void *o) {
    *(long *)o = (long)*(const int *)in * 10;
    return STREAM_EMIT;
  });
  assert(StreamCollect(s, &g_opts, &out) == COLLECT_OK);
  g_sink += (out.count ? ((long *)out.items)[out.count - 1] : 0) + (long)out.count;
  StreamResultFree(&g_opts, &out);
}

static void
fluent_collect_once(void)
{
  CollectionResult out;

  assert(FluentOfArray(g_in, g_len, sizeof(int))
           ->filter(^bool(const void *p) { return *(const int *)p > 3; })
           ->map(sizeof(long), ^StreamVerdict(const void *in, void *o) {
             *(long *)o = (long)*(const int *)in * 10;
             return STREAM_EMIT;
           })
           ->collect(g_opts, &out) == COLLECT_OK);
  g_sink += (out.count ? ((long *)out.items)[out.count - 1] : 0) + (long)out.count;
  StreamResultFree(&g_opts, &out);
}

static void
plain_count_once(void)
{
  size_t n;

  assert(StreamCount(StreamOfArray(g_in, g_len, sizeof(int)), &n) == COLLECT_OK);
  g_sink += (long)n;
}

static void
plain_count_filter_once(void)
{
  size_t n;
  Stream *s = StreamOfArray(g_in, g_len, sizeof(int));

  StreamFilter(s, ^bool(const void *p) { return *(const int *)p > 3; });
  assert(StreamCount(s, &n) == COLLECT_OK);
  g_sink += (long)n;
}

static void
fluent_count_once(void)
{
  size_t n;

  assert(FluentOfArray(g_in, g_len, sizeof(int))->count(&n) == COLLECT_OK);
  g_sink += (long)n;
}

static void
fluent_count_filter_once(void)
{
  size_t n;

  assert(FluentOfArray(g_in, g_len, sizeof(int))
           ->filter(^bool(const void *p) { return *(const int *)p > 3; })
           ->count(&n) == COLLECT_OK);
  g_sink += (long)n;
}

/* ---- timing ---- */
static double
bench(void (*fn)(void), int iters)
{
  for (int i = 0; i < WARM; i++) fn();

  uint64_t t0 = now_ns();
  for (int i = 0; i < iters; i++) fn();
  uint64_t t1 = now_ns();

  return (double)(t1 - t0) / (double)iters;
}

static int
cmp_double(const void *a, const void *b)
{
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double
median(double *v, int n)
{
  qsort(v, (size_t)n, sizeof *v, cmp_double);
  return v[n / 2];
}

static void
measure(const char *name, void (*fn)(void), double *out_n0, double *out_n10,
        unsigned long *out_alloc)
{
  double t0[REPS], t10[REPS];

  g_len = 0;
  for (int i = 0; i < REPS; i++) t0[i] = bench(fn, ITERS / 2);
  g_len = N;
  for (int i = 0; i < REPS; i++) t10[i] = bench(fn, ITERS);

  /* steady-state allocations per chain, after the pools are warm */
  for (int i = 0; i < WARM; i++) fn();
  alloc_reset();
  for (int i = 0; i < 100000; i++) fn();

  *out_n0    = median(t0, REPS);
  *out_n10   = median(t10, REPS);
  *out_alloc = alloc_total() / 100000;

  printf("%-26s %10.1f %12.1f %12.2f %14lu\n",
         name, *out_n0, *out_n10,
         (*out_n10 - *out_n0) / (double)N, *out_alloc);
  fflush(stdout);
}

int
main(void)
{
  printf("N=%d, %d iterations x %d reps, median, -O2, pinned CPU\n", N, ITERS, REPS);
  printf("%-26s %10s %12s %12s %14s\n",
         "mode", "N=0 ns", "N=10 ns", "ns/elem", "allocs/chain");
  printf("---------------------------------------------------------------------------------\n");

  for (int i = 0; i < N; i++) g_in[i] = i + 1;

  double a, b; unsigned long c;
  g_len = 0;
  measure("hand loop",                 hand_once,              &a, &b, &c);
  measure("descriptor collect",        desc_once,              &a, &b, &c);
  measure("plain stream collect",      plain_collect_once,     &a, &b, &c);
  measure("fluent stream collect",     fluent_collect_once,    &a, &b, &c);
  measure("plain stream count",        plain_count_once,       &a, &b, &c);
  measure("plain stream count+filter", plain_count_filter_once, &a, &b, &c);
  measure("fluent stream count",       fluent_count_once,      &a, &b, &c);
  measure("fluent stream count+filter", fluent_count_filter_once, &a, &b, &c);

  StreamThreadTrim();
  printf("sink=%ld\n", g_sink);
  return 0;
}
