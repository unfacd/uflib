/**
 * @file fence_redis_fluent.c
 * @brief Fence member uids as a fluent stream (clang blocks). See fence_redis_fluent.h.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Build: clang -fblocks; link -lBlocksRuntime off Apple platforms.
 */

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <standard_defs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis/hiredis.h>

#include "ufnet_core/thread_context_type.h"
#include "ufnet_msgcore/fence/fence.h"        /* CacheBackendLoadUserIdsForFence() */
#include "fence_redis_fluent.h"

extern __thread ThreadContext ufsrv_thread_context;

//region parsing
/* Is this Redis element a well-formed member uid? true => *uid_out is set.
 * (Same rules as the DescriptorV2 loader; kept separate so the two modules stay independent.) */
static bool
sParseUid(const redisReply *e, unsigned long *uid_out)
{
  if (IS_EMPTY(e) || IS_EMPTY(e->str)) return false;

  /* strtoul() silently accepts leading whitespace and +/-: require a digit up front */
  if (!isdigit((unsigned char)e->str[0])) return false;

  char *end = NULL;
  errno = 0;
  unsigned long uid = strtoul(e->str, &end, 10);

  if (errno != 0 || end == e->str || *end != '\0') return false;

  if (uid == 0) return false;                          /* uid 0 is never a member */

  *uid_out = uid;
  return true;
}

/* portable strdup (strdup is not in C17) */
static char *
sDupStr(const char *s)
{
  size_t n = strlen(s) + 1;
  char  *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}
//endregion

//region collect(): the only place strings are copied
/* Non-capturing, so these are global blocks. */
static StreamCopier sCopyToArena = ^bool(const void *item, void *dst, Arena *arena) {
  const FenceMemberEntry *in  = item;
  FenceMemberEntry       *out = dst;

  char *copy = ArenaStrdup(arena, in->raw);            /* a failed or aborted run rewinds the arena */
  if (!copy) return false;

  out->uid = in->uid;
  out->raw = copy;
  return true;
};

static StreamCopier sCopyToHeap = ^bool(const void *item, void *dst, Arena *arena) {
  (void)arena;
  const FenceMemberEntry *in  = item;
  FenceMemberEntry       *out = dst;

  char *copy = sDupStr(in->raw);
  if (!copy) return false;

  out->uid = in->uid;
  out->raw = copy;
  return true;
};

static void
sEntryDestroy(void *elem, void *ctx)
{
  (void)ctx;
  free(((FenceMemberEntry *)elem)->raw);
}

StreamCollectOpts
FenceMemberEntryOpts(bool use_arena)
{
  return (StreamCollectOpts){
    .elem_sz         = sizeof(FenceMemberEntry),
    .use_arena       = use_arena,
    .arena_chunk_sz  = 4096,
    .on_copy         = use_arena ? sCopyToArena : sCopyToHeap,
    .on_elem_destroy = use_arena ? NULL : sEntryDestroy,    /* arena strings die with the arena */
    .allow_empty     = false,                               /* every member filtered out => failure */
  };
}
//endregion

//region the source
/* The four source blocks capture NOTHING, so they are static blocks: copying them allocates nothing.
 * The only state a fence source needs is its fid, and that travels through StreamSource.arg. The
 * cursor is owned by the stream driver (it is the `pos` argument). So building this chain, running
 * it and tearing it down touches the heap zero times on a warm thread. */
static void *(^const sAcquire)(void *) = ^void *(void *arg) {
  CacheBackendLoadUserIdsForFence((unsigned long)(uintptr_t)arg);

  if (!THREAD_CONTEXT_UFSRV_RESULT_IS_SUCCESS_WITH_BACKEND_DATA) return NULL;

  redisReply *reply = THREAD_CONTEXT_UFSRV_RESULT_USERDATA;
  THREAD_CONTEXT_UFSRV_RESULT_USERDATA = NULL;          /* take ownership; release frees it */

  return reply;                                         /* NULL is reported as COLLECT_ERR_ACQUIRE */
};

static bool (^const sValidate)(void *, size_t *, void *) = ^bool(void *src, size_t *size_hint, void *arg) {
  (void)arg;
  const redisReply *r = src;

  if (r->type != REDIS_REPLY_ARRAY || r->elements == 0) return false;

  *size_hint = r->elements;
  return true;
};

static bool (^const sNext)(void *, size_t *, const void **, void *) =
  ^bool(void *src, size_t *pos, const void **item, void *arg) {
    (void)arg;
    const redisReply *r = src;

    if (*pos >= r->elements) return false;

    *item = r->element[(*pos)++];                       /* may be NULL: the parse stage drops it */
    return true;
  };

static void (^const sRelease)(void *, void *) = ^void(void *src, void *arg) {
  (void)arg;
  if (IS_PRESENT(src)) freeReplyObject((redisReply *)src);
};

/* Validates and parses one Redis element into a (borrowed) entry. Non-capturing: a static block. */
static StreamMapper sParseStage = ^StreamVerdict(const void *item, void *out) {
  const redisReply *e = item;
  unsigned long     uid;

  if (!sParseUid(e, &uid)) return STREAM_DROP;          /* malformed or uid 0 */

  FenceMemberEntry *entry = out;
  entry->uid = uid;
  entry->raw = e->str;                                  /* borrowed until collect() copies it */
  return STREAM_EMIT;
};

FStream *
FenceMemberUids(unsigned long fid)
{
  return FluentOfBlocks((StreamSource){
           .arg      = (void *)(uintptr_t)fid,
           .acquire  = sAcquire,
           .validate = sValidate,
           .next     = sNext,
           .release  = sRelease,
         })
         ->map(sizeof(FenceMemberEntry), sParseStage);
}
//endregion
