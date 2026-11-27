/* Build (clang, blocks on):
 *   clang -std=gnu17 -fblocks -Wall -Wextra -fsanitize=address,undefined \
 *         arena.c collection.c stream.c stream_fluent.c test_fluent.c \
 *         -lBlocksRuntime -o test_fluent && ./test_fluent
 *
 * Self-contained: exercises the stream engine and the fluent facade through the
 * generic sources. No backend or fence types are needed.
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collection/stream/stream_fluent.h"

#if defined(__has_feature)
#  if !__has_feature(blocks)
#    error "stream.h needs clang blocks: compile with -fblocks (and link -lBlocksRuntime off Apple platforms)"
#  endif
#endif

/* ================================================================== 1. generic stages (array source) */

static int g_ints[10];

static void test_stages(void)
{
  CollectionResult  out;
  StreamCollectOpts ints = { .elem_sz = sizeof(int) };       /* item points at an int: memcpy */

  assert(FluentOfArray(g_ints, 10, sizeof(int))->skip(2)->limit(3)->collect(ints, &out) == COLLECT_OK);
  const int *v = out.items;
  assert(out.count == 3 && v[0] == 3 && v[1] == 4 && v[2] == 5 && out.skipped == 2);
  StreamResultFree(&ints, &out);

  assert(FluentOfArray(g_ints, 10, sizeof(int))
           ->dropWhile(^bool(const void *p) { return *(const int *)p < 5; })
           ->collect(ints, &out) == COLLECT_OK);
  assert(out.count == 6 && ((int *)out.items)[0] == 5);
  StreamResultFree(&ints, &out);

  { __block int seen = 0, sum = 0;                           /* mutable captured state is __block */
    assert(FluentOfArray(g_ints, 10, sizeof(int))
             ->peek(^(const void *p) { seen++; sum += *(const int *)p; })
             ->collect(ints, &out) == COLLECT_OK);
    assert(seen == 10 && sum == 55);
    StreamResultFree(&ints, &out); }

  { StreamCollectOpts longs = { .elem_sz = sizeof(long) };   /* map changes the element type */
    assert(FluentOfArray(g_ints, 10, sizeof(int))
             ->map(sizeof(long), ^StreamVerdict(const void *in, void *o) {
               *(long *)o = *(const int *)in * 10L;
               return STREAM_EMIT; })
             ->filter(^bool(const void *p) { return *(const long *)p % 20 == 0; })
             ->skip(1)->limit(3)
             ->collect(longs, &out) == COLLECT_OK);
    const long *l = out.items;
    assert(out.count == 3 && l[0] == 40 && l[1] == 60 && l[2] == 80);
    StreamResultFree(&longs, &out);

    /* STREAM_EMIT_STOP emits then ends; STREAM_STOP ends without emitting */
    assert(FluentOfArray(g_ints, 10, sizeof(int))
             ->map(sizeof(long), ^StreamVerdict(const void *in, void *o) {
               *(long *)o = *(const int *)in;
               return *(const int *)in == 3 ? STREAM_EMIT_STOP : STREAM_EMIT; })
             ->collect(longs, &out) == COLLECT_OK && out.count == 3);
    StreamResultFree(&longs, &out);
    assert(FluentOfArray(g_ints, 10, sizeof(int))
             ->map(sizeof(long), ^StreamVerdict(const void *in, void *o) {
               *(long *)o = *(const int *)in;
               return *(const int *)in == 3 ? STREAM_STOP : STREAM_EMIT; })
             ->collect(longs, &out) == COLLECT_OK && out.count == 2);
    StreamResultFree(&longs, &out); }
}

/* ================================================================== 2. laziness (blocks source with a probe) */

typedef struct { int acquired, pulled, released; size_t n; } Probe;

/* A source with NO captured state: the probe travels through `arg`, the cursor is the driver's `pos`.
 * All four blocks are static blocks, so building this chain allocates nothing. */
static FStream *probe(Probe *p, size_t n)
{
  p->n = n;
  return FluentOfBlocks((StreamSource){
    .arg     = p,
    .acquire = ^void *(void *a)                                                  { ((Probe *)a)->acquired++; return a; },
    .next    = ^bool(void *src, size_t *pos, const void **item, void *a)         { (void)src; Probe *q = a;
                                                                                    if (*pos >= q->n) return false;
                                                                                    *item = &g_ints[(*pos)++]; q->pulled++; return true; },
    .release = ^void(void *src, void *a)                                         { (void)a; ((Probe *)src)->released++; },
  });
}

static void test_laziness(void)
{
  CollectionResult  out;
  StreamCollectOpts ints = { .elem_sz = sizeof(int) };
  Probe p;

  memset(&p, 0, sizeof p);
  { FStream *f = probe(&p, 10); f = f->limit(2);                         /* building runs nothing */
    assert(p.acquired == 0 && p.pulled == 0);
    assert(f->collect(ints, &out) == COLLECT_OK && out.count == 2 && p.pulled == 2 && p.released == 1);
    StreamResultFree(&ints, &out); }

  memset(&p, 0, sizeof p);
  assert(probe(&p, 10)->takeWhile(^bool(const void *x) { return *(const int *)x < 5; })->collect(ints, &out) == COLLECT_OK);
  assert(out.count == 4 && p.pulled == 5 && p.released == 1);            /* the failing item was pulled once */
  StreamResultFree(&ints, &out);

  /* limit counts what reaches it: a later stage dropping the last one still ends the stream */
  memset(&p, 0, sizeof p);
  assert(probe(&p, 10)->limit(3)->filter(^bool(const void *x) { return *(const int *)x % 2 == 0; })->collect(ints, &out) == COLLECT_OK);
  assert(out.count == 1 && ((int *)out.items)[0] == 2 && p.pulled == 3 && out.skipped == 2);
  StreamResultFree(&ints, &out);

  /* limit(0) never even opens the source */
  memset(&p, 0, sizeof p);
  { StreamCollectOpts e = { .elem_sz = sizeof(int), .allow_empty = true };
    assert(probe(&p, 10)->limit(0)->collect(e, &out) == COLLECT_OK && out.count == 0 && p.acquired == 0); }

  /* short-circuiting terminals */
  { bool r; size_t n;
    memset(&p, 0, sizeof p);
    assert(probe(&p, 10)->anyMatch(^bool(const void *x) { return *(const int *)x == 3; }, &r) == COLLECT_OK);
    assert(r && p.pulled == 3 && p.released == 1);

    memset(&p, 0, sizeof p);
    assert(probe(&p, 10)->allMatch(^bool(const void *x) { return *(const int *)x % 2 == 0; }, &r) == COLLECT_OK);
    assert(!r && p.pulled == 1);

    memset(&p, 0, sizeof p);
    assert(probe(&p, 10)->noneMatch(^bool(const void *x) { return *(const int *)x > 100; }, &r) == COLLECT_OK);
    assert(r && p.pulled == 10);

    memset(&p, 0, sizeof p);
    assert(probe(&p, 10)->count(&n) == COLLECT_OK && n == 10 && p.released == 1);

    __block long total = 0;
    assert(FluentOfArray(g_ints, 10, sizeof(int))->forEach(^(const void *x) { total += *(const int *)x; }) == COLLECT_OK);
    assert(total == 55);

    StreamPredicate yes = ^bool(const void *x) { (void)x; return true; };   /* empty-stream identities */
    assert(FluentOfArray(NULL, 0, 4)->anyMatch(yes, &r) == COLLECT_OK && !r);
    assert(FluentOfArray(NULL, 0, 4)->allMatch(yes, &r) == COLLECT_OK && r);
    assert(FluentOfArray(NULL, 0, 4)->noneMatch(yes, &r) == COLLECT_OK && r); }
}

/* ================================================================== 3. an existing descriptor as the source */

typedef struct { int acquired, released, pulled; size_t pos; } DescCtx;

static void *dAcq(void *c)  { ((DescCtx *)c)->acquired++; return c; }
static void  dRel(void *s, void *c) { (void)c; ((DescCtx *)s)->released++; }
static bool  dNext(const void *s, size_t *pos, const void **item, void *c)
{
  (void)c; (void)pos;
  DescCtx *d = (DescCtx *)s;
  if (d->pos >= 10) return false;
  *item = &g_ints[d->pos++]; d->pulled++;
  return true;
}

static void test_descriptor_source(void)
{
  const CollectionDescriptorV2 desc = { .on_acquire = dAcq, .on_release = dRel, .on_next = dNext };
  DescCtx           ctx = {0};
  CollectionResult  out;
  StreamCollectOpts ints = { .elem_sz = sizeof(int) };

  assert(FluentOfDescriptor(&desc, &ctx)->filter(^bool(const void *p) { return *(const int *)p > 6; })->limit(2)->collect(ints, &out) == COLLECT_OK);
  assert(out.count == 2 && ((int *)out.items)[0] == 7 && ctx.pulled == 8 && ctx.released == 1);
  StreamResultFree(&ints, &out);
}


/* ================================================================== 4. collect: heap/arena deep copies */

typedef struct { unsigned long uid; char *raw; } Entry;

static bool copy_entry(const void *item, void *dst, Arena *arena)
{
  const char *s = item;
  size_t      n = strlen(s) + 1;
  char       *copy;

  if (arena) {
    copy = ArenaStrdup(arena, s);
  }
  else {
    copy = malloc(n);
    if (copy) memcpy(copy, s, n);
  }

  if (!copy) return false;

  Entry *e = dst;
  e->uid = strtoul(s, NULL, 10);
  e->raw = copy;
  return true;
}

static void destroy_entry(void *elem, void *ctx)
{
  (void)ctx;
  free(((Entry *)elem)->raw);
}

/* A global block: its storage outlives every options struct built from it. */
static StreamCopier const kCopyEntry = ^bool(const void *item, void *dst, Arena *arena) {
  return copy_entry(item, dst, arena);
};

static StreamCollectOpts entry_opts(bool use_arena)
{
  StreamCollectOpts o = {
    .elem_sz        = sizeof(Entry),
    .use_arena      = use_arena,
    .arena_chunk_sz = 64,
    .on_copy        = kCopyEntry,
  };
  if (!use_arena) o.on_elem_destroy = destroy_entry;
  return o;
}

static void test_collect_copies(void)
{
  static const void *const items[] = { "10", "20", "30" };
  CollectionResult  out;
  StreamCollectOpts heap  = entry_opts(false);
  StreamCollectOpts arena = entry_opts(true);

  assert(FluentOfPointers(items, 3)->collect(heap, &out) == COLLECT_OK);
  assert(out.count == 3 && out.arena.head == NULL);
  { const Entry *e = out.items;
    assert(e[0].uid == 10 && e[1].uid == 20 && e[2].uid == 30);
    assert(strcmp(e[0].raw, "10") == 0 && strcmp(e[2].raw, "30") == 0);
    assert(e[0].raw != items[0]);                                   /* a real copy, not the borrowed string */
  }
  StreamResultFree(&heap, &out);
  assert(out.items == NULL && out.count == 0);

  assert(FluentOfPointers(items, 3)->collect(arena, &out) == COLLECT_OK);
  assert(out.count == 3 && ArenaUsed(&out.arena) > 0);
  { const Entry *e = out.items;
    assert(e[0].uid == 10 && e[2].uid == 30 && strcmp(e[2].raw, "30") == 0);
  }
  StreamResultFree(&arena, &out);
  assert(out.items == NULL && out.arena.head == NULL);
}

/* ================================================================== 5. collect policy */

static void test_collect_policy(void)
{
  CollectionResult  out;
  StreamCollectOpts ints = { .elem_sz = sizeof(int) };
  StreamPredicate   none = ^bool(const void *p) { (void)p; return false; };

  /* every item is dropped: empty is an error unless allowed */
  assert(FluentOfArray(g_ints, 10, sizeof(int))->filter(none)->collect(ints, &out) == COLLECT_ERR_EMPTY);
  assert(out.items == NULL && out.count == 0 && out.skipped == 10);

  ints.allow_empty = true;
  assert(FluentOfArray(g_ints, 10, sizeof(int))->filter(none)->collect(ints, &out) == COLLECT_OK);
  assert(out.items == NULL && out.count == 0 && out.skipped == 10);
  StreamResultFree(&ints, &out);

  ints.max_skipped = 2;
  assert(FluentOfArray(g_ints, 10, sizeof(int))->filter(none)->collect(ints, &out) == COLLECT_ERR_SKIP_LIMIT);
  assert(out.items == NULL && out.count == 0 && out.skipped == 3);
}

/* ================================================================== 6. collect failures */

static void test_collect_failure(void)
{
  static const void *const items[] = { "10", "20", "30" };
  CollectionResult  out;
  StreamCollectOpts heap = entry_opts(false);

  /* the copier fails on the second element: the first copy must be rolled back */
  { __block int calls = 0; StreamCopier real = heap.on_copy;
    StreamCollectOpts flaky = heap;
    flaky.on_copy = ^bool(const void *item, void *dst, Arena *a) {
      if (++calls == 2) return false;
      return real(item, dst, a);
    };
    assert(FluentOfPointers(items, 3)->collect(flaky, &out) == COLLECT_ERR_ALLOC);
    assert(out.items == NULL && out.count == 0); }

  /* bad stage and bad options surface at the terminal, outputs stay clean */
  assert(FluentOfPointers(items, 3)->filter(NULL)->collect(heap, &out) == COLLECT_ERR_BAD_SPEC);
  assert(out.items == NULL && out.count == 0);
  { StreamCollectOpts zero = {0};
    assert(FluentOfPointers(items, 3)->collect(zero, &out) == COLLECT_ERR_BAD_SPEC);
    assert(out.items == NULL && out.count == 0); }

  /* abort from a map stage */
  { StreamCollectOpts ints = { .elem_sz = sizeof(int) };
    assert(FluentOfArray(g_ints, 10, sizeof(int))
             ->map(sizeof(int), ^StreamVerdict(const void *in, void *o) {
               if (*(const int *)in == 3) return STREAM_ABORT;
               *(int *)o = *(const int *)in;
               return STREAM_EMIT; })
             ->collect(ints, &out) == COLLECT_ERR_ABORTED);
    assert(out.items == NULL && out.count == 0); }
}

/* ================================================================== 7. inert chains */

static void test_inert(void)
{
  CollectionResult  out;
  StreamCollectOpts ints = { .elem_sz = sizeof(int) };
  size_t            n    = 99;

  memset(&out, 0xAB, sizeof out);
  assert(StreamFluent(NULL)->filter(^bool(const void *p) { (void)p; return true; })
           ->skip(1)->limit(3)->collect(ints, &out) == COLLECT_ERR_BAD_SPEC);
  assert(out.items == NULL && out.count == 0 && out.skipped == 0);

  assert(StreamFluent(NULL)->count(&n) == COLLECT_ERR_BAD_SPEC && n == 0);
  StreamFluent(NULL)->close();
  StreamResultFree(&ints, &out);                                   /* safe on a cleared result */
}

/* ================================================================== 4b. the engine's fallback paths and pools */

static void test_large_pipelines(void)
{
  CollectionResult out; size_t n;

  /* more stages than the inline capacity: stages spill to the heap and run state moves off the stack */
  { __block int seen = 0;
    FStream *f = FluentOfArray(g_ints, 10, sizeof(int));
    for (int i = 0; i < 9; i++) f = f->peek(^(const void *p) { (void)p; seen++; });
    assert(f->count(&n) == COLLECT_OK);
    assert(n == 10 && seen == 90); }

  /* a map slot bigger than the on-stack scratch area: heap scratch fallback */
  { typedef struct { char pad[300]; int v; } Big;
    StreamCollectOpts bo = { .elem_sz = sizeof(Big) };
    assert(FluentOfArray(g_ints, 10, sizeof(int))
             ->map(sizeof(Big), ^StreamVerdict(const void *in, void *o) { ((Big *)o)->v = *(const int *)in * 7; return STREAM_EMIT; })
             ->filter(^bool(const void *p) { return ((const Big *)p)->v % 14 == 0; })
             ->collect(bo, &out) == COLLECT_OK);
    const Big *b = out.items;
    assert(out.count == 5 && b[0].v == 14 && b[4].v == 70);
    StreamResultFree(&bo, &out); }

  /* a map slot that is NOT re-zeroed per item: a block that writes everything every time is unaffected */
  { StreamCollectOpts li = { .elem_sz = sizeof(long) };
    assert(FluentOfArray(g_ints, 10, sizeof(int))
             ->map(sizeof(long), ^StreamVerdict(const void *in, void *o) { *(long *)o = *(const int *)in; return STREAM_EMIT; })
             ->collect(li, &out) == COLLECT_OK && ((long *)out.items)[9] == 10);
    StreamResultFree(&li, &out); }
}

/* Worker threads build chains and exit: their pools must be released by the thread-exit hook
 * (LeakSanitizer fails the run if the pooled objects of a dead thread leak). */
static void *worker(void *arg)
{
  (void)arg;
  size_t n; CollectionResult out; StreamCollectOpts ints = { .elem_sz = sizeof(int) };

  for (int i = 0; i < 200; i++) {
    assert(FluentOfArray(g_ints, 10, sizeof(int))->filter(^bool(const void *p) { return *(const int *)p > 3; })->count(&n) == COLLECT_OK && n == 7);
    assert(FluentOfArray(g_ints, 10, sizeof(int))->limit(2)->collect(ints, &out) == COLLECT_OK && out.count == 2);
    StreamResultFree(&ints, &out);
  }
  return NULL;
}

static void test_threads(void)
{
  pthread_t th[4];
  for (int i = 0; i < 4; i++) assert(pthread_create(&th[i], NULL, worker, NULL) == 0);
  for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
}


int main(void)
{
  for (int i = 0; i < 10; i++) g_ints[i] = i + 1;

  test_stages();
  test_laziness();
  test_descriptor_source();
  test_collect_copies();
  test_collect_policy();
  test_collect_failure();
  test_inert();
  test_large_pipelines();
  test_threads();
  StreamThreadTrim();                       /* release this thread's pools: nothing may be left allocated */

  puts("fluent streams: stages, laziness, descriptor source, heap/arena collect, "
       "policy, failures, inert chains, fallback paths, pools, threads: all passed");
  return 0;
}
