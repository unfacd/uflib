/*
 * test_fence_sample.c — runs the actual tests/collection/stream/fence_redis_fluent.c
 * against stub hiredis/ufnet headers, using the proposed fluent call shape:
 *
 *   FenceMemberUids(fid)->filter(...)->limit(50)->collect(opts, &out)
 *
 * The stubs in ./stubs replace the Redis reply and thread-context types; the
 * backend below builds a small in-memory reply, so no Redis server is needed.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hiredis/hiredis.h>
#include "ufnet_core/thread_context_type.h"
#include "ufnet_msgcore/fence/fence.h"
#include "fence_redis_fluent.h"

__thread ThreadContext ufsrv_thread_context;

/* 101, 202 and 303 are valid; the other five are malformed/null/zero. */
static const char *g_members[] = { "101", NULL, "bad", "202", "-7", "0", "303", "9x" };
static int g_loads;
static int g_freed;

static void free_reply(redisReply *r);

void
freeReplyObject(void *reply)
{
  g_freed++;
  free_reply(reply);
}

static void
free_reply(redisReply *r)
{
  if (!r) return;

  for (size_t i = 0; i < r->elements; i++) free_reply(r->element[i]);
  free(r->element);
  free(r->str);
  free(r);
}

void
CacheBackendLoadUserIdsForFence(unsigned long fid)
{
  (void)fid;
  g_loads++;

  redisReply *r = calloc(1, sizeof *r);
  r->type      = REDIS_REPLY_ARRAY;
  r->elements  = sizeof g_members / sizeof g_members[0];
  r->element   = calloc(r->elements, sizeof *r->element);

  for (size_t i = 0; i < r->elements; i++) {
    if (!g_members[i]) continue;

    redisReply *e = calloc(1, sizeof *e);
    e->type = REDIS_REPLY_STRING;
    e->len  = strlen(g_members[i]);
    e->str  = malloc(e->len + 1);
    memcpy(e->str, g_members[i], e->len + 1);
    r->element[i] = e;
  }

  ufsrv_thread_context.userdata = r;
  ufsrv_thread_context.ok       = 1;
}

static void
test_proposed_shape(void)
{
  StreamCollectOpts opts = FenceMemberEntryOpts(true);   /* strings copied into an arena */
  CollectionResult  out;
  const unsigned long requester = 202;

  g_loads = g_freed = 0;

  CollectResult rc = FenceMemberUids(7)                  /* source */
      ->filter(^bool(const void *p) {                    /* stage  */
        return ((const FenceMemberEntry *)p)->uid != requester;
      })
      ->limit(50)                                        /* stage  */
      ->collect(opts, &out);                             /* terminal: runs it */

  assert(rc == COLLECT_OK);

  const FenceMemberEntry *e = out.items;
  assert(out.count == 2);
  assert(e[0].uid == 101 && strcmp(e[0].raw, "101") == 0);
  assert(e[1].uid == 303 && strcmp(e[1].raw, "303") == 0);
  assert(e[0].raw != g_members[0]);                      /* a real copy, not the borrowed string */

  for (size_t i = 0; i < out.count; i++) {               /* the sample's use(e[i].uid, e[i].raw) */
    assert(e[i].uid != 0 && e[i].raw != NULL);
  }

  assert(g_loads == 1 && g_freed == 1);
  StreamResultFree(&opts, &out);
  assert(out.items == NULL && out.count == 0);
}

static void
test_heap_copy_and_terminals(void)
{
  StreamCollectOpts heap = FenceMemberEntryOpts(false);
  CollectionResult  out;
  size_t            n;
  bool              found = false;

  assert(FenceMemberUids(7)
           ->filter(^bool(const void *p) { return ((const FenceMemberEntry *)p)->uid == 303; })
           ->limit(1)
           ->collect(heap, &out) == COLLECT_OK);
  assert(out.count == 1 && ((const FenceMemberEntry *)out.items)[0].uid == 303);
  StreamResultFree(&heap, &out);

  assert(FenceMemberUids(7)->count(&n) == COLLECT_OK && n == 3);
  assert(FenceMemberUids(7)
           ->anyMatch(^bool(const void *p) { return ((const FenceMemberEntry *)p)->uid == 303; }, &found)
         == COLLECT_OK && found);
  assert(FenceMemberUids(7)
           ->anyMatch(^bool(const void *p) { return ((const FenceMemberEntry *)p)->uid == 404; }, &found)
         == COLLECT_OK && !found);
}

int
main(void)
{
  test_proposed_shape();
  test_heap_copy_and_terminals();

  StreamThreadTrim();
  puts("fence_redis_fluent.c (stub backend): source -> filter -> limit -> collect/arena, "
       "heap copy, count, anyMatch: all passed");
  return 0;
}
