/**
 * @file stream.h
 * @brief Lazy, single-pass streams over the collection primitives, using clang blocks.
 *
 * Stages (filter, map, peek, skip, limit, takeWhile, dropWhile) compose into a pipeline that a
 * terminal operation (collect, count, forEach, anyMatch, allMatch, noneMatch) runs. Items are pulled
 * one at a time and pushed through every stage, so limit()/takeWhile()/anyMatch() stop the source
 * early. See stream_fluent.h for the method-chaining front end.
 *
 * Reused from the collector: CollectionDescriptorV2 as a SOURCE (on_acquire, on_validate, on_next,
 * on_release), CollectResult, CollectionResult, CollectionResultFree, Arena.
 *
 * Semantics:
 *   - Single use: a terminal operation consumes the stream. StreamClose() discards an unrun one.
 *   - Stages BORROW. `item` points into the source or a map stage's scratch slot and is valid only
 *     for the duration of the callback. Only collect() copies. Source items may be NULL.
 *   - A map stage's `out` slot is scratch that persists between items: it is zeroed once when the run
 *     starts and NOT re-zeroed per item, so a map block must fully write `out` when it returns EMIT.
 *   - Null-safe chaining: a stage that cannot be added records an error that the terminal reports.
 *
 * COST MODEL (measured; see the benchmark notes in stream_fluent.h)
 *   - A chain makes NO internal heap allocation in steady state: Stream objects are recycled through
 *     a thread-local pool, up to STREAM_INLINE_STAGES stages are stored inline, and per-run state and
 *     map scratch live on the stack (heap only as a fallback for unusually large pipelines).
 *     A collect() terminal still allocates the result buffer it hands back to the caller; count()
 *     and the match/forEach terminals allocate nothing.
 *   - Blocks that capture nothing are static: Block_copy() of one is free. Blocks that capture
 *     variables cost one allocation per copy. So prefer non-capturing blocks, and pass small state
 *     to sources through StreamSource.arg instead of capturing it.
 *   - Per item there is no allocation: just one indirect call per stage.
 *   - The pools release themselves when a thread exits; StreamThreadTrim() releases them earlier.
 *
 * Build: clang -fblocks; link -lBlocksRuntime off Apple platforms.
 */
#ifndef STREAM_H
#define STREAM_H

#include <stdbool.h>
#include <stddef.h>

#include "collection/collection.h"

#if defined(__has_feature)
#  if !__has_feature(blocks)
#    error "stream.h needs clang blocks: compile with -fblocks (and link -lBlocksRuntime off Apple platforms)"
#  endif
#endif

#define STREAM_INLINE_STAGES 6        /* stages stored inline; more spill to the heap */

typedef struct Stream Stream;

/** What a map stage decides for one item. */
typedef enum {
  STREAM_EMIT,       /**< pass `out` downstream                                            */
  STREAM_DROP,       /**< discard this item, keep streaming                                */
  STREAM_STOP,       /**< discard this item and end the stream (successfully)              */
  STREAM_EMIT_STOP,  /**< pass `out` downstream, then end the stream                       */
  STREAM_ABORT       /**< fail the whole run with COLLECT_ERR_ABORTED                      */
} StreamVerdict;

typedef bool          (^StreamPredicate)(const void *item);
typedef StreamVerdict (^StreamMapper)(const void *in, void *out);   /* must fully write `out` on EMIT */
typedef void          (^StreamConsumer)(const void *item);

/** Deep-copies `item` into the collected slot `dst` (zeroed). `arena` is non-NULL only when
 *  use_arena is set. Return false if the copy failed (out of memory): the run fails with
 *  COLLECT_ERR_ALLOC and the collector rewinds whatever you allocated from the arena. */
typedef bool          (^StreamCopier)(const void *item, void *dst, Arena *arena);

/* ---- sources ------------------------------------------------------------------------- */

/** Source = the descriptor's on_acquire/on_validate/on_next/on_release, with `ctx` as passed to them
 *  (same lifecycle as CollectorRun: if on_acquire is NULL the source is ctx itself). */
Stream *StreamOfDescriptor(const CollectionDescriptorV2 *source, void *ctx);

/** Items are pointers to `n` consecutive elements of `stride` bytes starting at `base`. */
Stream *StreamOfArray(const void *base, size_t n, size_t stride);

/** Items are the `n` pointers themselves (e.g. a char *[] of strings; entries may be NULL). */
Stream *StreamOfPointers(const void *const *ptrs, size_t n);

/**
 * A source written with blocks. Every block receives `arg`, so a source needs NO captured state:
 * pass a pointer-sized value (a uid, a struct pointer) through `arg` and write the blocks as
 * non-capturing literals, which are static and free to copy. Nothing runs until a terminal operation.
 * The driver owns the cursor and passes it as `*pos` (starts at 0), like a descriptor's on_next.
 */
typedef struct StreamSource {
  void *arg;                                                          /**< passed to every block                     */
  void *(^acquire)(void *arg);                                        /**< optional. NULL result = COLLECT_ERR_ACQUIRE; if absent `src` is NULL */
  bool  (^validate)(void *src, size_t *size_hint, void *arg);         /**< optional. false = COLLECT_ERR_VALIDATE; may set a size hint */
  bool  (^next)(void *src, size_t *pos, const void **item, void *arg);/**< required. false = end of stream           */
  void  (^release)(void *src, void *arg);                             /**< optional. Exactly once if acquire() succeeded */
} StreamSource;

Stream *StreamOfBlocks(StreamSource source);

/* ---- intermediate operations (return the same stream, for chaining) ------------------------ */

Stream *StreamFilter(Stream *s, StreamPredicate keep);             /* keep items where keep() is true      */
Stream *StreamMap(Stream *s, size_t out_sz, StreamMapper map);     /* transform into an out_sz scratch slot */
Stream *StreamPeek(Stream *s, StreamConsumer observe);             /* observe without changing              */
Stream *StreamSkip(Stream *s, size_t n);                           /* drop the first n items reaching here  */
Stream *StreamLimit(Stream *s, size_t n);                          /* end the stream after n items          */
Stream *StreamTakeWhile(Stream *s, StreamPredicate pred);          /* end at the first item failing pred    */
Stream *StreamDropWhile(Stream *s, StreamPredicate pred);          /* drop while pred; then pass the rest   */

/* ---- terminal operations: each consumes the stream ----------------------------------------- */

typedef struct StreamCollectOpts {
  size_t elem_sz;           /**< size of one collected element. Required.                              */
  bool   use_arena;         /**< collected data may live in an Arena owned by the result              */
  size_t arena_chunk_sz;    /**< 0 = ARENA_DEFAULT_CHUNK_SZ                                           */
  StreamCopier on_copy;     /**< optional deep copy. NULL = memcpy(elem_sz) from the item, so the item
                                 must then point at >= elem_sz readable bytes (after a map stage of
                                 that size, or StreamOfArray with that stride) and must not be NULL */
  void (*on_elem_destroy)(void *elem, void *ctx);  /**< optional: frees heap data of one kept element   */
  void  *destroy_ctx;       /**< passed to on_elem_destroy                                            */
  size_t max_skipped;       /**< fail with COLLECT_ERR_SKIP_LIMIT past this many dropped items (0 = off) */
  bool   allow_empty;       /**< empty result is success with items = NULL, count = 0                 */
} StreamCollectOpts;

/** Same result shape and failure behaviour as CollectorRun(): `out` is always initialised; on
 *  failure it is clean; `out->skipped` counts items dropped by stages. */
CollectResult StreamCollect(Stream *s, const StreamCollectOpts *opts, CollectionResult *out);

/** Release a collected result. Pass the SAME opts used to collect (needed for on_elem_destroy). */
void StreamResultFree(const StreamCollectOpts *opts, CollectionResult *out);

CollectResult StreamCount(Stream *s, size_t *count);
CollectResult StreamForEach(Stream *s, StreamConsumer each);
CollectResult StreamAnyMatch(Stream *s, StreamPredicate pred, bool *result);    /* false on empty */
CollectResult StreamAllMatch(Stream *s, StreamPredicate pred, bool *result);    /* true on empty  */
CollectResult StreamNoneMatch(Stream *s, StreamPredicate pred, bool *result);   /* true on empty  */

/** Discard a stream that will not be run. NULL-safe. Never call it after a terminal operation. */
void StreamClose(Stream *s);

/* ---- per-thread pools ---------------------------------------------------------------------- */

/** Register a function to run when the calling thread exits, or when StreamThreadTrim() is called.
 *  Used by stream_fluent.c to release its own pool. At most a few distinct functions per thread. */
void StreamThreadCleanupRegister(void (*cleanup)(void));

/** Release the calling thread's pooled objects now. Optional: it also happens automatically at
 *  thread exit. Does not affect chains in flight. */
void StreamThreadTrim(void);

#endif /* STREAM_H */
