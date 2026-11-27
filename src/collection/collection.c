/**
 * @file collection.c
 * @brief collection
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 16/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#include "collection/collection.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define COLLECTOR_INITIAL_CAP 8

typedef struct {
  unsigned char *buf;
  size_t         cap;   /* capacity in elements */
  size_t         n;     /* kept elements        */
} Buf;

/* Ensure room for at least `need` elements. Geometric growth, overflow-checked. */
static bool
sReserve(Buf *b, size_t need, size_t elem_sz)
{
  if (need <= b->cap) return true;

  size_t ncap = b->cap ? b->cap : COLLECTOR_INITIAL_CAP;

  while (ncap < need) {
    if (ncap > SIZE_MAX / 2) return false;
    ncap *= 2;
  }

  if (ncap > SIZE_MAX / elem_sz) return false;

  unsigned char *p = realloc(b->buf, ncap * elem_sz);
  if (!p) return false;

  b->buf = p;
  b->cap = ncap;
  return true;
}

static void
sDestroyElems(const CollectionDescriptorV2 *s, unsigned char *buf, size_t n, void *ctx)
{
  if (!buf || !s->on_elem_destroy) return;
  for (size_t i = 0; i < n; i++) s->on_elem_destroy(buf + i * s->elem_sz, ctx);
}

CollectResult
CollectorRun(const CollectionDescriptorV2 *s, void *ctx, CollectionResult *out)
{
  if (!out) return COLLECT_ERR_BAD_SPEC;
  memset(out, 0, sizeof *out);

  if (!s || s->elem_sz == 0 || !s->on_next || !s->on_process) return COLLECT_ERR_BAD_SPEC;

  /* ---- on_acquire ---- */
  void *src = ctx;
  if (s->on_acquire) {
    src = s->on_acquire(ctx);
    if (!src) return COLLECT_ERR_ACQUIRE;
  }

  Buf           b       = {0};
  Arena         ar      = {0};
  Arena        *ap      = NULL;
  size_t        pos     = 0;
  size_t        skipped = 0;
  CollectResult rc      = COLLECT_OK;
  const void   *item    = NULL;

  if (s->use_arena) {
    ArenaInit(&ar, s->arena_chunk_sz);
    ap = &ar;
  }

  /* ---- source-level validation + size hint ---- */
  size_t hint = 0;
  if (s->on_validate && !s->on_validate(src, &hint, ctx)) {
    rc = COLLECT_ERR_VALIDATE;
    goto done;
  }
  if (hint > 0 && !sReserve(&b, hint, s->elem_sz)) {
    rc = COLLECT_ERR_ALLOC;
    goto done;
  }

  /* ---- iterate ---- */
  while ((s->limit == 0 || b.n < s->limit) && s->on_next(src, &pos, &item, ctx)) {
    bool skip = false;

    if (s->on_prefilter && !s->on_prefilter(item, ctx)) {
      skip = true;
    }
    else {
      if (!sReserve(&b, b.n + 1, s->elem_sz)) {
        rc = COLLECT_ERR_ALLOC;
        goto done;
      }

      unsigned char *slot = b.buf + b.n * s->elem_sz;
      memset(slot, 0, s->elem_sz);                 /* zeroed scratch for on_process() */

      ArenaMark mark = ArenaGetMark(ap);           /* {NULL,0} when ap is NULL */
      CollectVerdict v = s->on_process(item, slot, ap, ctx);

      if (v == COLLECT_TAKE) {
        b.n++;                                     /* the ONLY place a slot is consumed */
        continue;
      }

      if (ap) ArenaRewind(ap, mark);               /* reclaim whatever on_process() allocated */

      if (v == COLLECT_ABORT) {
        rc = COLLECT_ERR_ABORTED;
        goto done;
      }

      memset(slot, 0, s->elem_sz);                 /* COLLECT_SKIP (unknown verdicts treated as skip) */
      skip = true;
    }

    if (skip) {
      skipped++;
      if (s->max_skipped > 0 && skipped > s->max_skipped) {
        rc = COLLECT_ERR_SKIP_LIMIT;
        goto done;
      }
    }
  }

  /* ---- finalise ---- */
  if (b.n == 0) {
    if (!s->allow_empty) rc = COLLECT_ERR_EMPTY;
    goto done;                                     /* empty + allowed: items stay NULL, rc OK */
  }

  if (b.n < b.cap) {                               /* give back unused tail; failure is harmless */
    unsigned char *p = realloc(b.buf, b.n * s->elem_sz);
    if (p) { b.buf = p; b.cap = b.n; }
  }

  out->items = b.buf;                              /* ownership transferred */
  out->count = b.n;
  if (ap) out->arena = ar;

  b.buf = NULL;
  b.n   = 0;
  ap    = NULL;                                    /* arena now belongs to `out` */

done:
  if (rc != COLLECT_OK) sDestroyElems(s, b.buf, b.n, ctx);   /* elements first: they may read arena memory */
  free(b.buf);                                     /* NULL after a successful hand-over */
  if (ap) ArenaDestroy(ap);                        /* failure, or empty-but-allowed */

  out->skipped = skipped;

  if (s->on_acquire && s->on_release) s->on_release(src, ctx);

  return rc;
}

void
CollectionResultFree(const CollectionDescriptorV2 *s, CollectionResult *out, void *ctx)
{
  if (!out) return;

  if (s) sDestroyElems(s, out->items, out->count, ctx);

  free(out->items);
  ArenaDestroy(&out->arena);

  memset(out, 0, sizeof *out);
}

const char *
CollectorResultStr(CollectResult r)
{
  switch (r) {
    case COLLECT_OK:             return "ok";
    case COLLECT_ERR_BAD_SPEC:   return "bad spec";
    case COLLECT_ERR_ACQUIRE:    return "on_acquire failed";
    case COLLECT_ERR_VALIDATE:   return "on_validate failed";
    case COLLECT_ERR_ALLOC:      return "allocation failed";
    case COLLECT_ERR_ABORTED:    return "aborted by item";
    case COLLECT_ERR_SKIP_LIMIT: return "too many skipped items";
    case COLLECT_ERR_EMPTY:      return "empty result";
  }
  return "unknown";
}
