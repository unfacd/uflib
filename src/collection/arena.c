/**
 * @file arena.c
 * @brief arena
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

#include "collection/arena.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ArenaChunk {
  ArenaChunk    *next;   /* older chunk */
  size_t         cap;    /* payload capacity in bytes */
  size_t         used;   /* payload bytes consumed (incl. alignment padding) */
  unsigned char  data[]; /* payload */
};

static bool
sIsPow2(size_t x)
{
  return x != 0 && (x & (x - 1)) == 0;
}

/* Bump-allocate from one chunk; NULL if it does not fit. align is a power of two. */
static void *
sBump(ArenaChunk *c, size_t sz, size_t align)
{
  uintptr_t base    = (uintptr_t)c->data;
  uintptr_t cur     = base + c->used;
  uintptr_t aligned = (cur + (align - 1)) & ~(uintptr_t)(align - 1);
  size_t    off     = (size_t)(aligned - base);

  if (off > c->cap || sz > c->cap - off) return NULL;

  c->used = off + sz;
  return (void *)aligned;
}

void
ArenaInit(Arena *a, size_t chunk_sz)
{
  a->head     = NULL;
  a->chunk_sz = chunk_sz ? chunk_sz : ARENA_DEFAULT_CHUNK_SZ;
}

void *
ArenaAlloc(Arena *a, size_t sz, size_t align)
{
  if (!a) return NULL;
  if (sz == 0)    sz    = 1;                       /* always hand back a distinct pointer */
  if (align == 0) align = _Alignof(max_align_t);
  if (!sIsPow2(align)) return NULL;

  if (a->head) {
    void *p = sBump(a->head, sz, align);
    if (p) return p;
  }

  /* new chunk: room for the request plus worst-case alignment padding */
  if (sz > SIZE_MAX - align) return NULL;
  size_t need = sz + align;
  size_t cap  = need > a->chunk_sz ? need : a->chunk_sz;

  if (cap > SIZE_MAX - sizeof(ArenaChunk)) return NULL;

  ArenaChunk *c = malloc(sizeof(ArenaChunk) + cap);
  if (!c) return NULL;

  c->next = a->head;
  c->cap  = cap;
  c->used = 0;
  a->head = c;

  return sBump(c, sz, align);                      /* always fits by construction */
}

void *
ArenaAllocZ(Arena *a, size_t sz, size_t align)
{
  void *p = ArenaAlloc(a, sz, align);
  if (p) memset(p, 0, sz ? sz : 1);
  return p;
}

void *
ArenaMemdup(Arena *a, const void *src, size_t sz)
{
  if (!src && sz > 0) return NULL;

  void *p = ArenaAlloc(a, sz, 1);
  if (p && sz > 0) memcpy(p, src, sz);
  return p;
}

char *
ArenaStrdup(Arena *a, const char *s)
{
  if (!s) return NULL;
  return ArenaMemdup(a, s, strlen(s) + 1);
}

ArenaMark
ArenaGetMark(const Arena *a)
{
  ArenaMark m = { NULL, 0 };
  if (a && a->head) {
    m.chunk = a->head;
    m.used  = a->head->used;
  }
  return m;
}

void
ArenaRewind(Arena *a, ArenaMark m)
{
  if (!a) return;

  while (a->head != m.chunk) {                     /* drop chunks added after the mark */
    ArenaChunk *c = a->head;
    if (!c) return;                                /* invalid mark: nothing sensible left to do */
    a->head = c->next;
    free(c);
  }

  if (a->head) a->head->used = m.used;
}

size_t
ArenaUsed(const Arena *a)
{
  size_t total = 0;
  if (!a) return 0;
  for (const ArenaChunk *c = a->head; c; c = c->next) total += c->used;
  return total;
}

void
ArenaDestroy(Arena *a)
{
  if (!a) return;

  ArenaChunk *c = a->head;
  while (c) {
    ArenaChunk *next = c->next;
    free(c);
    c = next;
  }
  a->head = NULL;
}
