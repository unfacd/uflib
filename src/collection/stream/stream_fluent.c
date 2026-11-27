/**
 * @file stream_fluent.c
 * @brief Method-chaining facade over stream.h. See stream_fluent.h for the contract.
 *
 * The 14 method blocks of an FStream capture only `self`, and they are built ONCE per FStream object.
 * Objects are recycled through a thread-local pool, so a chain on a warm thread allocates nothing.
 */

#include "collection/stream/stream_fluent.h"

#include <Block.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define FLUENT_POOL_MAX 16

//region teardown and the pool
/* Releases every method block and frees the FStream. Block_release(NULL) is a no-op, so this is
 * also the cleanup for a half-built FStream. */
static void
sFree(FStream *f)
{
  Block_release(f->filter);
  Block_release(f->map);
  Block_release(f->peek);
  Block_release(f->skip);
  Block_release(f->limit);
  Block_release(f->takeWhile);
  Block_release(f->dropWhile);
  Block_release(f->collect);
  Block_release(f->count);
  Block_release(f->forEach);
  Block_release(f->anyMatch);
  Block_release(f->allMatch);
  Block_release(f->noneMatch);
  Block_release(f->close);
  free(f);
}

static __thread FStream *tl_pool;
static __thread unsigned tl_pool_n;
static __thread bool     tl_registered;

static void
sTrim(void)
{
  while (tl_pool) {
    FStream *f = tl_pool;
    tl_pool = f->pool_next_;
    sFree(f);
  }
  tl_pool_n = 0;
}

/* Retire a finished chain: keep its blocks alive and recycle the object. Called FIRST by every
 * terminal, before the pipeline runs, and only with values already read out of `f`; the executing
 * terminal block is never freed unless the pool is full, and then only after its arguments were
 * extracted by the caller. */
static void
sPark(FStream *f)
{
  f->s_ = NULL;

  if (tl_pool_n < FLUENT_POOL_MAX) {
    f->pool_next_ = tl_pool;
    tl_pool       = f;
    tl_pool_n++;

    if (!tl_registered) {
      tl_registered = true;
      StreamThreadCleanupRegister(sTrim);
    }
    return;
  }

  sFree(f);
}

/*
 * Terminal helpers are plain C functions on purpose. A terminal method is itself one of the blocks
 * that sFree() may release, so the block body hands everything it needs to a function as ARGUMENTS
 * (read before the call) and does not touch its own captured variables afterwards.
 */
static CollectResult
sCollect(FStream *f, StreamCollectOpts opts, CollectionResult *out)
{
  Stream *s = f->s_;
  sPark(f);
  return StreamCollect(s, &opts, out);
}

static CollectResult
sCount(FStream *f, size_t *count)
{
  Stream *s = f->s_;
  sPark(f);
  return StreamCount(s, count);
}

static CollectResult
sForEach(FStream *f, StreamConsumer each)
{
  Stream *s = f->s_;
  sPark(f);
  return StreamForEach(s, each);
}

static CollectResult
sAnyMatch(FStream *f, StreamPredicate pred, bool *result)
{
  Stream *s = f->s_;
  sPark(f);
  return StreamAnyMatch(s, pred, result);
}

static CollectResult
sAllMatch(FStream *f, StreamPredicate pred, bool *result)
{
  Stream *s = f->s_;
  sPark(f);
  return StreamAllMatch(s, pred, result);
}

static CollectResult
sNoneMatch(FStream *f, StreamPredicate pred, bool *result)
{
  Stream *s = f->s_;
  sPark(f);
  return StreamNoneMatch(s, pred, result);
}

static void
sClose(FStream *f)
{
  Stream *s = f->s_;
  sPark(f);
  StreamClose(s);
}
//endregion

//region the inert FStream
/* Returned when no stream could be created. Immutable and shared, so it is thread-safe and
 * never freed. Its blocks are non-capturing (global blocks). Outputs are cleared so a caller can
 * always release them. */
static FStream kInertFStream = {
  .filter    = ^FStream *(StreamPredicate p)               { (void)p; return &kInertFStream; },
  .map       = ^FStream *(size_t n, StreamMapper m)        { (void)n; (void)m; return &kInertFStream; },
  .peek      = ^FStream *(StreamConsumer c)                { (void)c; return &kInertFStream; },
  .skip      = ^FStream *(size_t n)                        { (void)n; return &kInertFStream; },
  .limit     = ^FStream *(size_t n)                        { (void)n; return &kInertFStream; },
  .takeWhile = ^FStream *(StreamPredicate p)               { (void)p; return &kInertFStream; },
  .dropWhile = ^FStream *(StreamPredicate p)               { (void)p; return &kInertFStream; },

  .collect   = ^CollectResult(StreamCollectOpts o, CollectionResult *out) {
    (void)o;
    if (out) memset(out, 0, sizeof *out);
    return COLLECT_ERR_BAD_SPEC;
  },
  .count     = ^CollectResult(size_t *n)                   { if (n) *n = 0; return COLLECT_ERR_BAD_SPEC; },
  .forEach   = ^CollectResult(StreamConsumer c)            { (void)c; return COLLECT_ERR_BAD_SPEC; },
  .anyMatch  = ^CollectResult(StreamPredicate p, bool *r)  { (void)p; (void)r; return COLLECT_ERR_BAD_SPEC; },
  .allMatch  = ^CollectResult(StreamPredicate p, bool *r)  { (void)p; (void)r; return COLLECT_ERR_BAD_SPEC; },
  .noneMatch = ^CollectResult(StreamPredicate p, bool *r)  { (void)p; (void)r; return COLLECT_ERR_BAD_SPEC; },
  .close     = ^{ },
};
//endregion

/* Builds a brand-new FStream with all 14 method blocks. Runs only when the pool is empty. */
static FStream *
sBuild(void)
{
  FStream *f = calloc(1, sizeof *f);
  if (!f) return NULL;

  /* Each method block captures `self` by value. The stream's sticky-error design means the
   * intermediate methods need no checking: a failure is reported by the terminal. */
  FStream *self = f;

  f->filter    = Block_copy(^FStream *(StreamPredicate p)        { StreamFilter(self->s_, p);        return self; });
  f->map       = Block_copy(^FStream *(size_t n, StreamMapper m) { StreamMap(self->s_, n, m);        return self; });
  f->peek      = Block_copy(^FStream *(StreamConsumer c)         { StreamPeek(self->s_, c);          return self; });
  f->skip      = Block_copy(^FStream *(size_t n)                 { StreamSkip(self->s_, n);          return self; });
  f->limit     = Block_copy(^FStream *(size_t n)                 { StreamLimit(self->s_, n);         return self; });
  f->takeWhile = Block_copy(^FStream *(StreamPredicate p)        { StreamTakeWhile(self->s_, p);     return self; });
  f->dropWhile = Block_copy(^FStream *(StreamPredicate p)        { StreamDropWhile(self->s_, p);     return self; });

  f->collect   = Block_copy(^CollectResult(StreamCollectOpts o, CollectionResult *out) { return sCollect(self, o, out); });
  f->count     = Block_copy(^CollectResult(size_t *n)                                  { return sCount(self, n); });
  f->forEach   = Block_copy(^CollectResult(StreamConsumer c)                           { return sForEach(self, c); });
  f->anyMatch  = Block_copy(^CollectResult(StreamPredicate p, bool *r)                 { return sAnyMatch(self, p, r); });
  f->allMatch  = Block_copy(^CollectResult(StreamPredicate p, bool *r)                 { return sAllMatch(self, p, r); });
  f->noneMatch = Block_copy(^CollectResult(StreamPredicate p, bool *r)                 { return sNoneMatch(self, p, r); });
  f->close     = Block_copy(^{ sClose(self); });

  if (!f->filter || !f->map || !f->peek || !f->skip || !f->limit || !f->takeWhile || !f->dropWhile ||
      !f->collect || !f->count || !f->forEach || !f->anyMatch || !f->allMatch || !f->noneMatch || !f->close) {
    sFree(f);                            /* NULL-safe on the blocks that did get created */
    return NULL;
  }

  return f;
}

FStream *
StreamFluent(Stream *s)
{
  if (!s) return &kInertFStream;

  FStream *f = tl_pool;

  if (f) {
    tl_pool = f->pool_next_;
    tl_pool_n--;
    f->pool_next_ = NULL;
  }
  else {
    f = sBuild();
    if (!f) { StreamClose(s); return &kInertFStream; }
  }

  f->s_ = s;
  return f;
}

//region chain starters
FStream *FluentOfArray(const void *base, size_t n, size_t stride)   { return StreamFluent(StreamOfArray(base, n, stride)); }
FStream *FluentOfPointers(const void *const *ptrs, size_t n)         { return StreamFluent(StreamOfPointers(ptrs, n)); }
FStream *FluentOfBlocks(StreamSource source)                         { return StreamFluent(StreamOfBlocks(source)); }
FStream *FluentOfDescriptor(const CollectionDescriptorV2 *source, void *ctx) { return StreamFluent(StreamOfDescriptor(source, ctx)); }
//endregion
