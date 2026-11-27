//
// Created by ayman on 16/11/2026.
//

/**
 * @file collection_descriptor_v2_type.h
 * @brief collection_descriptor_v2_type
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

#ifndef UFLIB_COLLECTION_DESCRIPTOR_V2_TYPE_H
#define UFLIB_COLLECTION_DESCRIPTOR_V2_TYPE_H

/*
 *generic "on_acquire -> iterate -> on_validate/filter/convert -> collect -> on_release" driver.
 *
 *   on_acquire()         get the source (optional; if NULL the source is ctx itself)
 *   on_validate()        source-level check + optional size hint (optional)
 *   loop:
 *     on_next()          advance the iterator
 *     on_prefilter()     cheap gate on the RAW item, no parsing (optional)
 *     on_process()       validate + filter + convert in ONE step, returns a verdict
 *   on_release()         called exactly once if on_acquire() succeeded (optional)
 *
 * A slot in the output array is consumed ONLY when on_process() returns COLLECT_TAKE, so the
 * output is always dense.
 *
 * Memory model for data an element needs to own (copies of strings etc.):
 *
 *   heap mode  (use_arena = false)  on_process() mallocs, on_elem_destroy() frees each
 *                                   element's data.
 *   arena mode (use_arena = true)   on_process() allocates from the Arena it is handed. The
 *                                   collector rewinds the arena automatically when an item is
 *                                   skipped or aborts, so on_process() never has to clean up.
 *                                   No on_elem_destroy() needed; everything dies with the arena.
 *
 * The output (array + arena) is returned in a CollectionResult and released with
 * CollectionResultFree(). Elements must not reference memory owned by the source, because the
 * source is released before CollectorRun() returns.
 */

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"

typedef enum {
  COLLECT_TAKE,   /**< dst slot is filled and valid: keep it (consumes a slot)            */
  COLLECT_SKIP,   /**< exclude this item (filtered out or malformed): no slot consumed    */
  COLLECT_ABORT   /**< invalidate the whole collection                                    */
} CollectVerdict;

typedef enum {
  COLLECT_OK = 0,
  COLLECT_ERR_BAD_SPEC,    /**< spec or arguments unusable                                */
  COLLECT_ERR_ACQUIRE,     /**< on_acquire() returned NULL                                */
  COLLECT_ERR_VALIDATE,    /**< on_validate() rejected the source                         */
  COLLECT_ERR_ALLOC,       /**< out of memory (or size overflow)                          */
  COLLECT_ERR_ABORTED,     /**< on_process() returned COLLECT_ABORT                       */
  COLLECT_ERR_SKIP_LIMIT,  /**< more than max_skipped items were skipped                  */
  COLLECT_ERR_EMPTY        /**< nothing collected and allow_empty is false                */
} CollectResult;

typedef struct CollectionDescriptorV2 {
  /** Size of one output element. Required. For by-pointer collections use sizeof(void *). */
  size_t elem_sz;

  /* ---- source lifecycle ------------------------------------------------------------ */

  /** Optional. Returns an opaque source or NULL on failure. If NULL, the source is `ctx`. */
  void *(*on_acquire)(void *ctx);

  /** Optional. Called exactly once if on_acquire() succeeded, on success and failure paths. */
  void  (*on_release)(void *src, void *ctx);

  /** Optional. Source-level precondition (type check, emptiness...). Return false to reject.
   *  May set *size_hint to the expected element count (pre-sizes the buffer; not trusted). */
  bool  (*on_validate)(const void *src, size_t *size_hint, void *ctx);

  /* ---- iteration -------------------------------------------------------------------- */

  /** Required. Yield the next item. *pos starts at 0 and is owned by the callback. Return
   *  false at end of source. The item may be NULL if the source can contain NULLs:
   *  on_process() then decides what to do with it. */
  bool  (*on_next)(const void *src, size_t *pos, const void **item, void *ctx);

  /* ---- per item --------------------------------------------------------------------- */

  /** Optional. Cheap gate on the raw item. Return false to skip without calling on_process(). */
  bool  (*on_prefilter)(const void *item, void *ctx);

  /** Required. Validate, filter and convert in one step.
   *
   *  `dst`   zeroed scratch slot of elem_sz bytes. Fill it and return COLLECT_TAKE to keep it.
   *  `arena` the collector's arena in arena mode, NULL otherwise.
   *
   *  Arena mode: allocate freely; on SKIP/ABORT the collector rewinds the arena for you.
   *  Heap mode:  on SKIP/ABORT you must not leave allocations behind (dst is discarded without
   *              on_elem_destroy), so allocate after your last SKIP point, or free before returning. */
  CollectVerdict (*on_process)(const void *item, void *dst, Arena *arena, void *ctx);

  /** Optional. Releases resources owned by one KEPT element (not arena memory). Used for rollback
   *  on failure and by CollectionResultFree(). Leave NULL for plain values / arena-only data. */
  void  (*on_elem_destroy)(void *elem, void *ctx);

  /* ---- policy ----------------------------------------------------------------------- */

  bool   use_arena;        /**< enable arena mode                                              */
  size_t arena_chunk_sz;   /**< arena chunk size (0 = ARENA_DEFAULT_CHUNK_SZ)                  */
  size_t max_skipped;      /**< fail with COLLECT_ERR_SKIP_LIMIT past this many skips (0 = unlimited) */
  size_t limit;            /**< stop after collecting this many elements (0 = unlimited)       */
  bool   allow_empty;      /**< true: empty result is success with items = NULL, count = 0     */
} CollectionDescriptorV2;

typedef struct CollectionResult {
  void  *items;     /**< dense array of `count` elements, malloc'd (NULL if empty)          */
  size_t count;     /**< number of elements                                                 */
  size_t skipped;   /**< items excluded (valid on success and failure)                      */
  Arena  arena;     /**< backing store for element data in arena mode; zeroed otherwise     */
} CollectionResult;


#endif //UFLIB_COLLECTION_DESCRIPTOR_V2_TYPE_H
