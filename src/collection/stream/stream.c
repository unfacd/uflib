/**
 * @file stream.c
 * @brief Lazy, single-pass streams over the collection primitives (clang blocks).
 *
 * See stream.h for the contract and the cost model. In short: no heap allocation per chain in steady
 * state (pooled Stream objects, inline stages, stack run-state) and none per item.
 */

#include "collection/stream/stream.h"

#include <Block.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_INITIAL_CAP     8
#define STREAM_POOL_MAX        16
#define STREAM_SCRATCH_INLINE  256          /* bytes of map scratch kept on the stack per run */
#define STREAM_MAX_CLEANUPS    4

typedef enum { SRC_DESC, SRC_ARRAY, SRC_PTRS, SRC_BLOCKS } SrcKind;

typedef enum {
  ST_FILTER, ST_MAP, ST_PEEK, ST_SKIP, ST_LIMIT, ST_TAKE_WHILE, ST_DROP_WHILE
} StageKind;

typedef struct {
  StageKind kind;
  union {
    StreamPredicate pred;     /* filter, takeWhile, dropWhile */
    StreamMapper    map;      /* map                          */
    StreamConsumer  peek;     /* peek                         */
  } fn;                       /* Block_copy'd: static (free) if non-capturing, heap otherwise */
  size_t n;                   /* skip / limit count */
  size_t out_sz;              /* map scratch size   */
} Stage;

struct Stream {
  Stream                       *next_free;  /* thread-local pool link */
  SrcKind                       kind;
  const CollectionDescriptorV2 *desc;       /* SRC_DESC */
  void                         *ctx;        /* SRC_DESC */
  const void                   *base;       /* SRC_ARRAY / SRC_PTRS */
  size_t                        n_items;
  size_t                        stride;
  StreamSource                  blk;        /* SRC_BLOCKS */
  Stage                        *stages;     /* inl, or a heap array once more than STREAM_INLINE_STAGES */
  size_t                        n_stages;
  size_t                        cap_stages;
  Stage                         inl[STREAM_INLINE_STAGES];
  CollectResult                 err;        /* first builder error wins; reported by the terminal op */
  bool                          empty;      /* limit(0): nothing can ever pass */
};

//region per-thread cleanup registry
static pthread_key_t     sKey;
static pthread_once_t    sKeyOnce = PTHREAD_ONCE_INIT;
static __thread void   (*tl_cleanups[STREAM_MAX_CLEANUPS])(void);
static __thread unsigned tl_ncleanups;

void
StreamThreadTrim(void)
{
  for (unsigned i = 0; i < tl_ncleanups; i++) tl_cleanups[i]();
}

static void sOnThreadExit(void *unused) { (void)unused; StreamThreadTrim(); }
static void sMakeKey(void)              { pthread_key_create(&sKey, sOnThreadExit); }

void
StreamThreadCleanupRegister(void (*cleanup)(void))
{
  for (unsigned i = 0; i < tl_ncleanups; i++) if (tl_cleanups[i] == cleanup) return;
  if (tl_ncleanups >= STREAM_MAX_CLEANUPS) return;

  pthread_once(&sKeyOnce, sMakeKey);
  pthread_setspecific(sKey, (void *)1);            /* a non-NULL value makes the destructor run at thread exit */
  tl_cleanups[tl_ncleanups++] = cleanup;
}
//endregion

//region lifecycle and the Stream pool
static __thread Stream  *tl_pool;
static __thread unsigned tl_pool_n;
static __thread bool     tl_pool_registered;

static void
sTrimPool(void)
{
  while (tl_pool) {
    Stream *s = tl_pool;
    tl_pool = s->next_free;
    free(s);
  }
  tl_pool_n = 0;
}

static void
sReleaseBlocks(Stream *s)
{
  for (size_t i = 0; i < s->n_stages; i++) {
    Stage *st = &s->stages[i];
    switch (st->kind) {
      case ST_FILTER:
      case ST_TAKE_WHILE:
      case ST_DROP_WHILE: Block_release(st->fn.pred); break;
      case ST_MAP:        Block_release(st->fn.map);  break;
      case ST_PEEK:       Block_release(st->fn.peek); break;
      case ST_SKIP:
      case ST_LIMIT:      break;
    }
  }

  if (s->kind == SRC_BLOCKS) {                                 /* Block_release(NULL) is a no-op */
    Block_release(s->blk.acquire);
    Block_release(s->blk.validate);
    Block_release(s->blk.next);
    Block_release(s->blk.release);
  }
}

static void
sDestroyStream(Stream *s)
{
  if (!s) return;

  sReleaseBlocks(s);

  if (s->stages != s->inl) free(s->stages);

  if (tl_pool_n < STREAM_POOL_MAX) {                           /* recycle: the next chain on this thread allocates nothing */
    s->next_free = tl_pool;
    tl_pool      = s;
    tl_pool_n++;

    if (!tl_pool_registered) {
      tl_pool_registered = true;
      StreamThreadCleanupRegister(sTrimPool);
    }
    return;
  }

  free(s);
}

void
StreamClose(Stream *s)
{
  sDestroyStream(s);
}

static Stream *
sNewStream(SrcKind kind)
{
  Stream *s = tl_pool;

  if (s) {
    tl_pool = s->next_free;
    tl_pool_n--;
  }
  else {
    s = malloc(sizeof *s);
    if (!s) return NULL;
  }

  s->next_free  = NULL;
  s->kind       = kind;
  s->desc       = NULL;
  s->ctx        = NULL;
  s->base       = NULL;
  s->n_items    = 0;
  s->stride     = 0;
  memset(&s->blk, 0, sizeof s->blk);
  s->stages     = s->inl;
  s->n_stages   = 0;
  s->cap_stages = STREAM_INLINE_STAGES;
  s->err        = COLLECT_OK;
  s->empty      = false;
  return s;
}
//endregion

//region sources
Stream *
StreamOfDescriptor(const CollectionDescriptorV2 *source, void *ctx)
{
  if (!source || !source->on_next) return NULL;

  Stream *s = sNewStream(SRC_DESC);
  if (!s) return NULL;

  s->desc = source;
  s->ctx  = ctx;
  return s;
}

Stream *
StreamOfArray(const void *base, size_t n, size_t stride)
{
  if ((!base && n > 0) || stride == 0) return NULL;

  Stream *s = sNewStream(SRC_ARRAY);
  if (!s) return NULL;

  s->base    = base;
  s->n_items = n;
  s->stride  = stride;
  return s;
}

Stream *
StreamOfPointers(const void *const *ptrs, size_t n)
{
  if (!ptrs && n > 0) return NULL;

  Stream *s = sNewStream(SRC_PTRS);
  if (!s) return NULL;

  s->base    = ptrs;
  s->n_items = n;
  return s;
}

Stream *
StreamOfBlocks(StreamSource source)
{
  if (!source.next) return NULL;

  Stream *s = sNewStream(SRC_BLOCKS);
  if (!s) return NULL;

  s->blk.arg      = source.arg;
  s->blk.acquire  = source.acquire  ? Block_copy(source.acquire)  : NULL;   /* a non-capturing block: no allocation */
  s->blk.validate = source.validate ? Block_copy(source.validate) : NULL;
  s->blk.next     = Block_copy(source.next);
  s->blk.release  = source.release  ? Block_copy(source.release)  : NULL;

  if (!s->blk.next || (source.acquire && !s->blk.acquire) ||
      (source.validate && !s->blk.validate) || (source.release && !s->blk.release)) {
    sDestroyStream(s);                                          /* releases whichever copies succeeded */
    return NULL;
  }

  return s;
}

static inline bool
sPull(const Stream *s, void *src, size_t *pos, const void **item)
{
  switch (s->kind) {
    case SRC_ARRAY:
      if (*pos >= s->n_items) return false;
      *item = (const unsigned char *)s->base + *pos * s->stride;
      (*pos)++;
      return true;

    case SRC_PTRS:
      if (*pos >= s->n_items) return false;
      *item = ((const void *const *)s->base)[*pos];
      (*pos)++;
      return true;

    case SRC_BLOCKS:
      return s->blk.next(src, pos, item, s->blk.arg);

    case SRC_DESC:
      return s->desc->on_next(src, pos, item, s->ctx);
  }
  return false;
}
//endregion

//region intermediate operations
/* Reserve the next stage slot. It only becomes part of the stream when the caller increments
 * n_stages, so a failure half way through leaves nothing behind to release. */
static Stage *
sAddStage(Stream *s, StageKind kind)
{
  if (s->err != COLLECT_OK) return NULL;

  if (s->n_stages == s->cap_stages) {
    size_t ncap = s->cap_stages * 2;
    if (ncap > SIZE_MAX / sizeof(Stage)) { s->err = COLLECT_ERR_ALLOC; return NULL; }

    Stage *p;
    if (s->stages == s->inl) {                                  /* spill from inline storage to the heap */
      p = malloc(ncap * sizeof *p);
      if (p) memcpy(p, s->inl, s->n_stages * sizeof *p);
    }
    else {
      p = realloc(s->stages, ncap * sizeof *p);
    }
    if (!p) { s->err = COLLECT_ERR_ALLOC; return NULL; }

    s->stages     = p;
    s->cap_stages = ncap;
  }

  Stage *st = &s->stages[s->n_stages];
  st->kind   = kind;
  st->n      = 0;
  st->out_sz = 0;
  return st;
}

static Stream *
sAddPredicateStage(Stream *s, StageKind kind, StreamPredicate pred)
{
  if (!s) return NULL;
  if (s->err != COLLECT_OK) return s;
  if (!pred) { s->err = COLLECT_ERR_BAD_SPEC; return s; }

  Stage *st = sAddStage(s, kind);
  if (!st) return s;

  StreamPredicate owned = Block_copy(pred);
  if (!owned) { s->err = COLLECT_ERR_ALLOC; return s; }

  st->fn.pred = owned;
  s->n_stages++;
  return s;
}

Stream *StreamFilter(Stream *s, StreamPredicate keep)    { return sAddPredicateStage(s, ST_FILTER,     keep); }
Stream *StreamTakeWhile(Stream *s, StreamPredicate pred) { return sAddPredicateStage(s, ST_TAKE_WHILE, pred); }
Stream *StreamDropWhile(Stream *s, StreamPredicate pred) { return sAddPredicateStage(s, ST_DROP_WHILE, pred); }

Stream *
StreamMap(Stream *s, size_t out_sz, StreamMapper map)
{
  if (!s) return NULL;
  if (s->err != COLLECT_OK) return s;
  if (!map || out_sz == 0) { s->err = COLLECT_ERR_BAD_SPEC; return s; }

  Stage *st = sAddStage(s, ST_MAP);
  if (!st) return s;

  StreamMapper owned = Block_copy(map);
  if (!owned) { s->err = COLLECT_ERR_ALLOC; return s; }

  st->fn.map = owned;
  st->out_sz = out_sz;
  s->n_stages++;
  return s;
}

Stream *
StreamPeek(Stream *s, StreamConsumer observe)
{
  if (!s) return NULL;
  if (s->err != COLLECT_OK) return s;
  if (!observe) { s->err = COLLECT_ERR_BAD_SPEC; return s; }

  Stage *st = sAddStage(s, ST_PEEK);
  if (!st) return s;

  StreamConsumer owned = Block_copy(observe);
  if (!owned) { s->err = COLLECT_ERR_ALLOC; return s; }

  st->fn.peek = owned;
  s->n_stages++;
  return s;
}

Stream *
StreamSkip(Stream *s, size_t n)
{
  if (!s) return NULL;

  Stage *st = sAddStage(s, ST_SKIP);
  if (!st) return s;

  st->n = n;
  s->n_stages++;
  return s;
}

Stream *
StreamLimit(Stream *s, size_t n)
{
  if (!s) return NULL;

  Stage *st = sAddStage(s, ST_LIMIT);
  if (!st) return s;

  st->n = n;
  s->n_stages++;
  if (n == 0) s->empty = true;               /* never open the source at all */
  return s;
}
//endregion

//region driver
/* Terminal operations plug in as a sink. Embedded as the FIRST member of each sink struct. */
typedef struct SinkBase {
  CollectResult (*accept)(struct SinkBase *self, const void *item, bool *stop);
  void          (*hint)(struct SinkBase *self, size_t n);   /* optional: expected element count */
  size_t         *fast_count;                               /* optional: bump this inline instead of calling accept (count) */
  size_t          max_skipped;                              /* 0 = unlimited; enforced by the driver */
} SinkBase;

typedef struct {
  size_t         seen;          /* skip / limit counter */
  bool           dropping;      /* dropWhile: still in the dropping phase */
  unsigned char *scratch;       /* map output slot, carved from the run's scratch area */
} StageRun;

#define PF_DROPPED 1u           /* the item never reached the sink */
#define PF_STOP    2u           /* the stream is over after this item */
#define PF_ERROR   4u           /* *rc holds the failure */

/*
 * Pushes ONE item through every stage and, if it survives, into the sink. A limit reaching its
 * quota ends the stream even if a later stage drops that last item.
 */
static inline unsigned
sPush(const Stream *s, StageRun *rs, const void *item, SinkBase *sink, CollectResult *rc)
{
  bool         stop_after = false;
  const size_t n          = s->n_stages;

  for (size_t i = 0; i < n; i++) {
    const Stage *st = &s->stages[i];
    StageRun    *r  = &rs[i];

    switch (st->kind) {
      case ST_FILTER:
        if (!st->fn.pred(item)) goto drop;
        break;

      case ST_PEEK:
        st->fn.peek(item);
        break;

      case ST_SKIP:
        if (r->seen < st->n) { r->seen++; goto drop; }
        break;

      case ST_LIMIT:
        if (r->seen >= st->n) { stop_after = true; goto drop; }   /* quota already met */
        r->seen++;
        if (r->seen == st->n) stop_after = true;                  /* this is the last one */
        break;

      case ST_TAKE_WHILE:
        if (!st->fn.pred(item)) { stop_after = true; goto drop; }
        break;

      case ST_DROP_WHILE:
        if (r->dropping) {
          if (st->fn.pred(item)) goto drop;
          r->dropping = false;
        }
        break;

      case ST_MAP:
        switch (st->fn.map(item, r->scratch)) {
          case STREAM_ABORT:     *rc = COLLECT_ERR_ABORTED; return PF_ERROR;
          case STREAM_DROP:      goto drop;
          case STREAM_STOP:      stop_after = true; goto drop;
          case STREAM_EMIT_STOP: stop_after = true; break;
          case STREAM_EMIT:      break;
        }
        item = r->scratch;                                        /* downstream sees the mapped value */
        break;
    }
  }

  if (sink->fast_count) {                                         /* count(): no call, no bookkeeping */
    (*sink->fast_count)++;
    return stop_after ? PF_STOP : 0u;
  }

  {
    bool          sink_stop = false;
    CollectResult e         = sink->accept(sink, item, &sink_stop);

    if (e != COLLECT_OK) { *rc = e; return PF_ERROR; }
    return (stop_after || sink_stop) ? PF_STOP : 0u;
  }

drop:
  return PF_DROPPED | (stop_after ? PF_STOP : 0u);
}

/* Runs the whole pipeline and CONSUMES the stream. Per-run state lives on the stack. */
static CollectResult
sRun(Stream *s, SinkBase *sink, size_t *skipped_out)
{
  CollectResult  rc       = s->err;
  void          *src      = NULL;
  bool           acquired = false;
  size_t         skipped  = 0;
  size_t         hint     = 0;
  size_t         pos      = 0;
  const void    *item     = NULL;

  StageRun       rs_inl[STREAM_INLINE_STAGES];
  _Alignas(16) unsigned char scratch_inl[STREAM_SCRATCH_INLINE];
  StageRun      *rs           = rs_inl;
  unsigned char *scratch_heap = NULL;
  StageRun      *rs_heap      = NULL;

  if (rc != COLLECT_OK) goto out;

  if (s->empty) goto out;                       /* limit(0): nothing can pass, so do not even open the source */

  /* ---- source: same lifecycle as CollectorRun ---- */
  if (s->kind == SRC_DESC) {
    src = s->ctx;

    if (s->desc->on_acquire) {
      src = s->desc->on_acquire(s->ctx);
      if (!src) { rc = COLLECT_ERR_ACQUIRE; goto out; }
      acquired = true;
    }

    if (s->desc->on_validate && !s->desc->on_validate(src, &hint, s->ctx)) {
      rc = COLLECT_ERR_VALIDATE;
      goto out;
    }
  }
  else if (s->kind == SRC_BLOCKS) {
    if (s->blk.acquire) {
      src = s->blk.acquire(s->blk.arg);
      if (!src) { rc = COLLECT_ERR_ACQUIRE; goto out; }
      acquired = true;
    }

    if (s->blk.validate && !s->blk.validate(src, &hint, s->blk.arg)) {
      rc = COLLECT_ERR_VALIDATE;
      goto out;
    }
  }
  else {
    hint = s->n_items;
  }

  if (sink->hint && hint > 0) sink->hint(sink, hint);

  /* ---- per-run stage state: on the stack, heap only for unusually large pipelines ---- */
  {
    const size_t n = s->n_stages;
    size_t       scratch_total = 0;

    for (size_t i = 0; i < n; i++) {
      if (s->stages[i].kind == ST_MAP) scratch_total += (s->stages[i].out_sz + 15u) & ~(size_t)15u;
    }

    unsigned char *scratch = scratch_inl;

    if (n > STREAM_INLINE_STAGES) {
      rs_heap = malloc(n * sizeof *rs_heap);
      if (!rs_heap) { rc = COLLECT_ERR_ALLOC; goto out; }
      rs = rs_heap;
    }
    if (scratch_total > STREAM_SCRATCH_INLINE) {
      scratch_heap = aligned_alloc(16, scratch_total);
      if (!scratch_heap) { rc = COLLECT_ERR_ALLOC; goto out; }
      scratch = scratch_heap;
    }

    memset(scratch, 0, scratch_total);          /* zeroed ONCE per run; map blocks fully write `out` */

    for (size_t i = 0; i < n; i++) {
      rs[i].seen     = 0;
      rs[i].dropping = true;
      rs[i].scratch  = NULL;

      if (s->stages[i].kind == ST_MAP) {
        rs[i].scratch = scratch;
        scratch      += (s->stages[i].out_sz + 15u) & ~(size_t)15u;
      }
    }
  }

  /* ---- pull / push ---- */
  while (sPull(s, src, &pos, &item)) {
    unsigned f = sPush(s, rs, item, sink, &rc);

    if (f & PF_ERROR) break;

    if (f & PF_DROPPED) {
      skipped++;
      if (sink->max_skipped > 0 && skipped > sink->max_skipped) {
        rc = COLLECT_ERR_SKIP_LIMIT;
        break;
      }
    }

    if (f & PF_STOP) break;
  }

out:
  free(scratch_heap);
  free(rs_heap);

  if (acquired) {
    if (s->kind == SRC_DESC   && s->desc->on_release) s->desc->on_release(src, s->ctx);
    if (s->kind == SRC_BLOCKS && s->blk.release)      s->blk.release(src, s->blk.arg);
  }

  *skipped_out = skipped;
  sDestroyStream(s);
  return rc;
}
//endregion

//region collect
typedef struct {
  SinkBase                 base;
  const StreamCollectOpts *o;
  unsigned char           *buf;
  size_t                   cap;
  size_t                   n;
  Arena                    ar;
  Arena                   *ap;
} CollectSink;

static bool
sReserve(CollectSink *c, size_t need)
{
  if (need <= c->cap) return true;

  size_t ncap = c->cap ? c->cap : STREAM_INITIAL_CAP;

  while (ncap < need) {
    if (ncap > SIZE_MAX / 2) return false;
    ncap *= 2;
  }

  if (ncap > SIZE_MAX / c->o->elem_sz) return false;

  unsigned char *p = realloc(c->buf, ncap * c->o->elem_sz);
  if (!p) return false;

  c->buf = p;
  c->cap = ncap;
  return true;
}

static void
sCollectHint(SinkBase *self, size_t n)
{
  (void)sReserve((CollectSink *)self, n);        /* only a pre-size: failure is harmless */
}

static CollectResult
sCollectAccept(SinkBase *self, const void *item, bool *stop)
{
  (void)stop;
  CollectSink *c = (CollectSink *)self;
  size_t       esz = c->o->elem_sz;

  if (!sReserve(c, c->n + 1)) return COLLECT_ERR_ALLOC;

  unsigned char *slot = c->buf + c->n * esz;
  memset(slot, 0, esz);

  if (c->o->on_copy) {
    ArenaMark mark = ArenaGetMark(c->ap);                  /* {NULL,0} when there is no arena */

    if (!c->o->on_copy(item, slot, c->ap)) {
      if (c->ap) ArenaRewind(c->ap, mark);
      memset(slot, 0, esz);
      return COLLECT_ERR_ALLOC;
    }
  }
  else {
    if (!item) return COLLECT_ERR_BAD_SPEC;                /* nothing to memcpy from */
    memcpy(slot, item, esz);
  }

  c->n++;
  return COLLECT_OK;
}

CollectResult
StreamCollect(Stream *s, const StreamCollectOpts *o, CollectionResult *out)
{
  if (!out) { StreamClose(s); return COLLECT_ERR_BAD_SPEC; }
  memset(out, 0, sizeof *out);

  if (!s) return COLLECT_ERR_BAD_SPEC;
  if (!o || o->elem_sz == 0) { StreamClose(s); return COLLECT_ERR_BAD_SPEC; }

  CollectSink c = {0};
  c.base.accept      = sCollectAccept;
  c.base.hint        = sCollectHint;
  c.base.max_skipped = o->max_skipped;
  c.o                = o;

  if (o->use_arena) {
    ArenaInit(&c.ar, o->arena_chunk_sz);
    c.ap = &c.ar;
  }

  size_t        skipped = 0;
  CollectResult rc      = sRun(s, &c.base, &skipped);        /* consumes s */

  if (rc == COLLECT_OK && c.n == 0 && !o->allow_empty) rc = COLLECT_ERR_EMPTY;

  if (rc != COLLECT_OK) {
    if (c.buf && o->on_elem_destroy) {                       /* elements first: they may read arena memory */
      for (size_t i = 0; i < c.n; i++) o->on_elem_destroy(c.buf + i * o->elem_sz, o->destroy_ctx);
    }
    free(c.buf);
    if (c.ap) ArenaDestroy(c.ap);
  }
  else if (c.n == 0) {                                       /* empty but allowed */
    free(c.buf);
    if (c.ap) ArenaDestroy(c.ap);
  }
  else {
    if (c.n < c.cap) {                                       /* give back the unused tail */
      unsigned char *p = realloc(c.buf, c.n * o->elem_sz);
      if (p) c.buf = p;
    }

    out->items = c.buf;
    out->count = c.n;
    if (c.ap) out->arena = c.ar;
  }

  out->skipped = skipped;
  return rc;
}

void
StreamResultFree(const StreamCollectOpts *o, CollectionResult *out)
{
  if (!out) return;

  /* reuse the collector's release path: it only needs elem_sz and on_elem_destroy */
  CollectionDescriptorV2 d;
  memset(&d, 0, sizeof d);

  if (o) {
    d.elem_sz         = o->elem_sz;
    d.on_elem_destroy = o->on_elem_destroy;
  }

  CollectionResultFree(&d, out, o ? o->destroy_ctx : NULL);
}
//endregion

//region other terminal operations
typedef struct { SinkBase base; size_t n; } CountSink;

static CollectResult
sCountAccept(SinkBase *self, const void *item, bool *stop)
{
  (void)item; (void)stop;
  ((CountSink *)self)->n++;
  return COLLECT_OK;
}

CollectResult
StreamCount(Stream *s, size_t *count)
{
  if (!count) { StreamClose(s); return COLLECT_ERR_BAD_SPEC; }
  *count = 0;
  if (!s) return COLLECT_ERR_BAD_SPEC;

  CountSink k = { .base = { .accept = sCountAccept } };
  k.base.fast_count = &k.n;                                  /* the driver bumps this inline */

  size_t        skipped;
  CollectResult rc = sRun(s, &k.base, &skipped);

  if (rc == COLLECT_OK) *count = k.n;
  return rc;
}

typedef struct { SinkBase base; StreamConsumer each; } EachSink;

static CollectResult
sEachAccept(SinkBase *self, const void *item, bool *stop)
{
  (void)stop;
  ((EachSink *)self)->each(item);
  return COLLECT_OK;
}

CollectResult
StreamForEach(Stream *s, StreamConsumer each)
{
  if (!each) { StreamClose(s); return COLLECT_ERR_BAD_SPEC; }
  if (!s) return COLLECT_ERR_BAD_SPEC;

  EachSink k = { .base = { .accept = sEachAccept }, .each = each };
  size_t   skipped;

  return sRun(s, &k.base, &skipped);
}

typedef enum { MATCH_ANY, MATCH_ALL, MATCH_NONE } MatchMode;
typedef struct { SinkBase base; StreamPredicate pred; MatchMode mode; bool result; } MatchSink;

static CollectResult
sMatchAccept(SinkBase *self, const void *item, bool *stop)
{
  MatchSink *m   = (MatchSink *)self;
  bool       hit = m->pred(item);

  switch (m->mode) {
    case MATCH_ANY:  if (hit)  { m->result = true;  *stop = true; } break;
    case MATCH_ALL:  if (!hit) { m->result = false; *stop = true; } break;
    case MATCH_NONE: if (hit)  { m->result = false; *stop = true; } break;
  }
  return COLLECT_OK;
}

static CollectResult
sMatch(Stream *s, StreamPredicate pred, bool *result, MatchMode mode)
{
  if (!pred || !result) { StreamClose(s); return COLLECT_ERR_BAD_SPEC; }
  if (!s) return COLLECT_ERR_BAD_SPEC;

  MatchSink k = { .base = { .accept = sMatchAccept }, .pred = pred, .mode = mode,
                  .result = (mode != MATCH_ANY) };           /* vacuous truth on an empty stream */
  size_t        skipped;
  CollectResult rc = sRun(s, &k.base, &skipped);

  if (rc == COLLECT_OK) *result = k.result;
  return rc;
}

CollectResult StreamAnyMatch(Stream *s, StreamPredicate pred, bool *result)  { return sMatch(s, pred, result, MATCH_ANY);  }
CollectResult StreamAllMatch(Stream *s, StreamPredicate pred, bool *result)  { return sMatch(s, pred, result, MATCH_ALL);  }
CollectResult StreamNoneMatch(Stream *s, StreamPredicate pred, bool *result) { return sMatch(s, pred, result, MATCH_NONE); }
//endregion
