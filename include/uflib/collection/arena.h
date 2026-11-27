//
// Created by ayman on 16/11/2026.
//

/**
 * @file arena.h
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

#ifndef UFLIB_ARENA_H
#define UFLIB_ARENA_H

/*
 * arena.h - small chunked bump allocator (C17).
 *
 * - Allocations are pointer-stable: the arena grows by adding chunks, never by realloc.
 * - Everything is released at once with ArenaDestroy().
 * - Stack-style rollback with ArenaGetMark()/ArenaRewind(): allocations made after a mark
 *   are discarded (and any chunks added since are freed). Marks must be rewound in LIFO order.
 * - An Arena is a plain value with no self-references, so it can be copied/moved by assignment
 *   (the original must then not be used or destroyed).
 */
#include <stddef.h>

#define ARENA_DEFAULT_CHUNK_SZ 4096u

typedef struct ArenaChunk ArenaChunk;

typedef struct Arena {
  ArenaChunk *head;      /**< newest chunk (allocation happens here)  */
  size_t      chunk_sz;  /**< preferred payload size of a new chunk   */
} Arena;

typedef struct ArenaMark {
  ArenaChunk *chunk;
  size_t      used;
} ArenaMark;

/** chunk_sz == 0 selects ARENA_DEFAULT_CHUNK_SZ. Allocates nothing until first use. */
void   ArenaInit(Arena *a, size_t chunk_sz);

/** align must be a power of two (0 = max_align_t). Returns NULL on OOM / bad args.
 *  Memory is uninitialised. A request larger than chunk_sz gets a chunk of its own. */
void  *ArenaAlloc(Arena *a, size_t sz, size_t align);

/** Same as ArenaAlloc but zero-filled. */
void  *ArenaAllocZ(Arena *a, size_t sz, size_t align);

void  *ArenaMemdup(Arena *a, const void *src, size_t sz);

/** Copies a NUL-terminated string into the arena. NULL in -> NULL out. */
char  *ArenaStrdup(Arena *a, const char *s);

ArenaMark ArenaGetMark(const Arena *a);

/** Discard everything allocated since `m` was taken. */
void   ArenaRewind(Arena *a, ArenaMark m);

/** Bytes handed out (including alignment padding) across live chunks. Mostly for tests/diagnostics. */
size_t ArenaUsed(const Arena *a);

/** Frees every chunk. Safe on a zeroed or already destroyed Arena. */
void   ArenaDestroy(Arena *a);

#endif //UFLIB_ARENA_H
