/* Build: cc -std=c17 -Wall -Wextra -pedantic -fsanitize=address,undefined \
 *           arena.c collection.c test_collection.c -o test_collection && ./test_collection */
#include "collection/collection.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ arena tests */

static void test_arena(void)
{
  Arena a;
  ArenaInit(&a, 64);

  /* alignment */
  char *c = ArenaAlloc(&a, 1, 1);
  assert(c);
  uint64_t *u = ArenaAlloc(&a, sizeof *u, _Alignof(uint64_t));
  assert(u && ((uintptr_t)u % _Alignof(uint64_t)) == 0);
  void *p64 = ArenaAlloc(&a, 10, 64);
  assert(p64 && ((uintptr_t)p64 % 64) == 0);
  assert(ArenaAlloc(&a, 8, 3) == NULL);                 /* non power-of-two alignment */

  /* oversized request gets its own chunk and stays usable */
  char *big = ArenaAllocZ(&a, 1000, 1);
  assert(big && big[0] == 0 && big[999] == 0);
  memset(big, 'x', 1000);

  /* pointer stability: earlier allocations survive growth */
  *c = 'q'; *u = 0xdeadbeef;
  for (int i = 0; i < 100; i++) assert(ArenaAlloc(&a, 50, 1));
  assert(*c == 'q' && *u == 0xdeadbeef);

  /* strdup / memdup */
  char *s = ArenaStrdup(&a, "hello");
  assert(s && strcmp(s, "hello") == 0);
  assert(ArenaStrdup(&a, NULL) == NULL);

  /* mark / rewind across chunk boundaries */
  size_t before = ArenaUsed(&a);
  ArenaMark m = ArenaGetMark(&a);
  for (int i = 0; i < 50; i++) assert(ArenaAlloc(&a, 40, 1));
  assert(ArenaUsed(&a) > before);
  ArenaRewind(&a, m);
  assert(ArenaUsed(&a) == before);
  assert(strcmp(s, "hello") == 0);

  /* rewind from an empty arena frees everything */
  Arena e; ArenaInit(&e, 32);
  ArenaMark em = ArenaGetMark(&e);
  for (int i = 0; i < 20; i++) assert(ArenaAlloc(&e, 20, 1));
  ArenaRewind(&e, em);
  assert(e.head == NULL && ArenaUsed(&e) == 0);
  ArenaDestroy(&e);

  ArenaDestroy(&a);
  ArenaDestroy(&a);                                     /* idempotent */
}

/* ------------------------------------------------------------ fake string source */

typedef struct { const char **v; size_t n; int released; } Src;

static void *sAcq(void *ctx) { return ctx; }
static void  sRel(void *src, void *ctx) { (void)ctx; ((Src *)src)->released++; }
static bool  sVal(const void *src, size_t *hint, void *ctx) { (void)ctx; *hint = ((const Src *)src)->n; return true; }
static void *sAcqFail(void *ctx) { (void)ctx; return NULL; }
static bool  sValReject(const void *src, size_t *hint, void *ctx) { (void)src; (void)hint; (void)ctx; return false; }
static bool  sNext(const void *src, size_t *pos, const void **item, void *ctx)
{
  (void)ctx;
  const Src *s = src;
  if (*pos >= s->n) return false;
  *item = (const void *)s->v[(*pos)++];                 /* may be NULL */
  return true;
}

typedef enum { P_OK, P_BAD, P_FILTERED } Parse;

static Parse sParse(const char *str, unsigned long *v)
{
  if (!str || !isdigit((unsigned char)str[0])) return P_BAD;
  char *end; errno = 0;
  unsigned long x = strtoul(str, &end, 10);
  if (errno || *end != '\0') return P_BAD;
  if (x == 0) return P_FILTERED;
  *v = x;
  return P_OK;
}

static int g_abort_on_bad;                              /* 1: malformed => ABORT, 0: malformed => SKIP */

/* ------------------------------------------------- plain-value elements (no copies) */

static CollectVerdict sProcVal(const void *item, void *dst, Arena *ar, void *ctx)
{
  (void)ctx;
  assert(ar == NULL);                                   /* arena must not be passed when disabled */
  unsigned long v;
  switch (sParse(item, &v)) {
    case P_OK:       *(unsigned long *)dst = v; return COLLECT_TAKE;
    case P_FILTERED: return COLLECT_SKIP;
    default:         return g_abort_on_bad ? COLLECT_ABORT : COLLECT_SKIP;
  }
}

static CollectionDescriptorV2 specVal(void)
{
  return (CollectionDescriptorV2){ .elem_sz = sizeof(unsigned long), .on_acquire = sAcq, .on_release = sRel,
                          .on_validate = sVal, .on_next = sNext, .on_process = sProcVal };
}

static void test_plain(void)
{
  CollectionResult o;

  { /* mixed: malformed excluded, dense and in order */
    const char *in[] = { "10", NULL, "abc", "20", "12x", "-5", "0", "30" };
    Src s = { in, 8, 0 }; CollectionDescriptorV2 sp = specVal(); g_abort_on_bad = 0;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_OK);
    unsigned long *u = o.items;
    assert(o.count == 3 && o.skipped == 5 && u[0] == 10 && u[1] == 20 && u[2] == 30);
    assert(s.released == 1);
    CollectionResultFree(&sp, &o, NULL);
    assert(o.items == NULL && o.count == 0);
  }
  { /* abort */
    const char *in[] = { "10", "bad", "30" };
    Src s = { in, 3, 0 }; CollectionDescriptorV2 sp = specVal(); g_abort_on_bad = 1;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_ERR_ABORTED);
    assert(o.items == NULL && o.count == 0 && s.released == 1);
  }
  { /* empty: error unless allowed */
    const char *in[] = { NULL, "x" };
    Src s = { in, 2, 0 }; CollectionDescriptorV2 sp = specVal(); g_abort_on_bad = 0;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_ERR_EMPTY);
    sp.allow_empty = true;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_OK && o.items == NULL && o.count == 0);
    CollectionResultFree(&sp, &o, NULL);
  }
  { /* skip limit */
    const char *in[] = { "1", "x", "y", "z" };
    Src s = { in, 4, 0 }; CollectionDescriptorV2 sp = specVal(); sp.max_skipped = 2; g_abort_on_bad = 0;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_ERR_SKIP_LIMIT && s.released == 1);
  }
  { /* no size hint => must grow; then limit */
    const char *in[100]; char store[100][4];
    for (int i = 0; i < 100; i++) { snprintf(store[i], 4, "%d", i + 1); in[i] = store[i]; }
    Src s = { in, 100, 0 }; CollectionDescriptorV2 sp = specVal(); g_abort_on_bad = 0;
    sp.on_validate = NULL;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_OK && o.count == 100);
    assert(((unsigned long *)o.items)[99] == 100);
    CollectionResultFree(&sp, &o, NULL);
    sp.limit = 7;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_OK && o.count == 7);
    CollectionResultFree(&sp, &o, NULL);
  }
  { /* on_acquire fails: on_release must NOT be called */
    Src s = { NULL, 0, 0 }; CollectionDescriptorV2 sp = specVal(); sp.on_acquire = sAcqFail;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_ERR_ACQUIRE && s.released == 0);
  }
  { /* no on_acquire/on_release: the source is ctx itself, on_release is never called */
    const char *in[] = { "5" };
    Src s = { in, 1, 0 }; CollectionDescriptorV2 sp = specVal(); sp.on_acquire = NULL; g_abort_on_bad = 0;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_OK && o.count == 1 && s.released == 0);
    CollectionResultFree(&sp, &o, NULL);
  }
  { /* on_validate rejects the source */
    const char *in[] = { "5" };
    Src s = { in, 1, 0 }; CollectionDescriptorV2 sp = specVal(); sp.on_validate = sValReject;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_ERR_VALIDATE && s.released == 1);
  }
}

/* ---------------------------------------- elements with retained copies (heap/arena) */

typedef struct { unsigned long v; char *raw; } Entry;

static char *sDup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }

static CollectVerdict sProcHeap(const void *item, void *dst, Arena *ar, void *ctx)
{
  (void)ctx;
  assert(ar == NULL);
  unsigned long v;
  Parse r = sParse(item, &v);
  if (r == P_FILTERED) return COLLECT_SKIP;
  if (r == P_BAD)      return g_abort_on_bad ? COLLECT_ABORT : COLLECT_SKIP;
  char *c = sDup(item);                                 /* allocate only after the last SKIP point */
  if (!c) return COLLECT_ABORT;
  ((Entry *)dst)->v = v; ((Entry *)dst)->raw = c;
  return COLLECT_TAKE;
}

static void sDestroyEntry(void *e, void *ctx) { (void)ctx; free(((Entry *)e)->raw); }

static CollectVerdict sProcArena(const void *item, void *dst, Arena *ar, void *ctx)
{
  (void)ctx;
  assert(ar != NULL);
  (void)ArenaAlloc(ar, 40, 1);                          /* scratch taken BEFORE validation: collector must reclaim on skip */
  unsigned long v;
  Parse r = sParse(item, &v);
  if (r == P_FILTERED) return COLLECT_SKIP;
  if (r == P_BAD)      return g_abort_on_bad ? COLLECT_ABORT : COLLECT_SKIP;
  char *c = ArenaStrdup(ar, item);
  if (!c) return COLLECT_ABORT;
  ((Entry *)dst)->v = v; ((Entry *)dst)->raw = c;
  return COLLECT_TAKE;
}

static void test_entries(void)
{
  const char *in[] = { "10", NULL, "abc", "2000", "12x", "0", "30", "-1", "456789" };
  const char *want[] = { "10", "2000", "30", "456789" };
  size_t want_bytes = 0;
  for (size_t i = 0; i < 4; i++) want_bytes += strlen(want[i]) + 1;

  CollectionResult o;

  { /* heap */
    Src s = { in, 9, 0 };
    CollectionDescriptorV2 sp = { .elem_sz = sizeof(Entry), .on_acquire = sAcq, .on_release = sRel, .on_validate = sVal,
                         .on_next = sNext, .on_process = sProcHeap, .on_elem_destroy = sDestroyEntry };
    g_abort_on_bad = 0;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_OK && o.count == 4 && o.skipped == 5);
    Entry *e = o.items;
    for (size_t i = 0; i < 4; i++) assert(strcmp(e[i].raw, want[i]) == 0);
    assert(o.arena.head == NULL);
    CollectionResultFree(&sp, &o, NULL);

    g_abort_on_bad = 1;                                 /* abort after some copies were kept: rollback must free them */
    Src s2 = { in, 9, 0 };
    assert(CollectorRun(&sp, &s2, &o) == COLLECT_ERR_ABORTED && o.items == NULL && s2.released == 1);
  }
  { /* arena */
    Src s = { in, 9, 0 };
    CollectionDescriptorV2 sp = { .elem_sz = sizeof(Entry), .on_acquire = sAcq, .on_release = sRel, .on_validate = sVal,
                         .on_next = sNext, .on_process = sProcArena, .use_arena = true, .arena_chunk_sz = 64 };
    g_abort_on_bad = 0;
    assert(CollectorRun(&sp, &s, &o) == COLLECT_OK && o.count == 4 && o.skipped == 5);
    Entry *e = o.items;
    for (size_t i = 0; i < 4; i++) assert(strcmp(e[i].raw, want[i]) == 0);
    /* A TAKE keeps everything on_process() allocated, so each kept item retains its 40-byte
     * scratch plus its string. Nothing from the 5 skipped items may remain: that is the
     * automatic rewind. */
    assert(ArenaUsed(&o.arena) == want_bytes + 4 * 40);
    CollectionResultFree(&sp, &o, NULL);
    assert(o.arena.head == NULL);

    g_abort_on_bad = 1;
    Src s2 = { in, 9, 0 };
    assert(CollectorRun(&sp, &s2, &o) == COLLECT_ERR_ABORTED && o.items == NULL && o.arena.head == NULL);
    assert(s2.released == 1);

    /* empty + allowed in arena mode must not leak the arena */
    const char *none[] = { NULL, "zz" };
    Src s3 = { none, 2, 0 }; g_abort_on_bad = 0; sp.allow_empty = true;
    assert(CollectorRun(&sp, &s3, &o) == COLLECT_OK && o.count == 0 && o.arena.head == NULL);
  }
}

int main(void)
{
  test_arena();
  test_plain();
  test_entries();
  puts("all arena + collector tests passed");
  return 0;
}
