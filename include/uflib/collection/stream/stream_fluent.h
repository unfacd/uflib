/**
 * @file stream_fluent.h
 * @brief Method-chaining syntax for streams:   source(...)->filter(...)->limit(50)->collect(...)
 *
 * Every method is a clang BLOCK stored in the struct that captured the stream when it was created,
 * which is what lets C drop the explicit `self` argument:
 *
 *   CollectionResult out;
 *   CollectResult rc = FenceMemberUids(fid)                                   // a source (see fence_redis_fluent.h)
 *       ->filter(^bool(const void *p) { return ((const FenceMemberEntry *)p)->uid != me; })
 *       ->limit(50)
 *       ->collect(FenceMemberEntryOpts(true), &out);                          // terminal: runs it all
 *
 *   size_t n;      FenceMemberUids(fid)->filter(isActive)->count(&n);
 *   bool   found;  FenceMemberUids(fid)->anyMatch(^bool(const void *p) { ... }, &found);
 *
 * A chain starts from one of the FluentOf...() constructors below (or a wrapper that returns an
 * FStream, like FenceMemberUids).
 *
 * Rules:
 *   - A chain is single use. The terminal method (collect, count, forEach, anyMatch, allMatch,
 *     noneMatch) runs the pipeline and then frees the FStream. Do not touch the FStream afterwards.
 *     To abandon a chain that you will not run, call ->close().
 *   - Never NULL: if a stream could not be created (OOM, bad source) you get an inert FStream whose
 *     intermediate methods return it unchanged and whose terminals return COLLECT_ERR_BAD_SPEC with
 *     their outputs cleared. Chains therefore never crash and need no null checks.
 *   - Block arguments are Block_copy'd by the intermediate methods, so they may capture locals
 *     (mutable state must be __block). Terminal-method blocks are used immediately and not copied.
 *   - collect() takes its options BY VALUE so they can be written inline or returned by a helper.
 *     Release a collected result with StreamResultFree(&opts, &out) using the same options.
 *   - Cost: no internal allocation in steady state. FStream objects, with their 14 method blocks
 *     already built, are recycled through a thread-local pool (and so are the underlying streams),
 *     so a chain allocates nothing internally once the thread is warm. The only per-chain allocations
 *     left are blocks YOU pass that capture variables (one each) and the result buffer that
 *     collect() hands back. Non-capturing blocks are static and free; prefer them.
 *   - After a terminal method the FStream is parked in the pool: never touch it again.
 *
 * Build: clang -fblocks; link -lBlocksRuntime off Apple platforms.
 */
#ifndef STREAM_FLUENT_H
#define STREAM_FLUENT_H

#if defined(__has_feature)
#  if !__has_feature(blocks)
#    error "stream.h needs clang blocks: compile with -fblocks (and link -lBlocksRuntime off Apple platforms)"
#  endif
#endif

#include "collection/stream/stream.h"

typedef struct FStream FStream;

struct FStream {
  /* intermediate: return the same FStream so calls chain */
  FStream      *(^filter)(StreamPredicate keep);
  FStream      *(^map)(size_t out_sz, StreamMapper mapper);
  FStream      *(^peek)(StreamConsumer observe);
  FStream      *(^skip)(size_t n);
  FStream      *(^limit)(size_t n);
  FStream      *(^takeWhile)(StreamPredicate pred);
  FStream      *(^dropWhile)(StreamPredicate pred);

  /* terminal: run the pipeline, consume (free) the FStream */
  CollectResult (^collect)(StreamCollectOpts opts, CollectionResult *out);
  CollectResult (^count)(size_t *count);
  CollectResult (^forEach)(StreamConsumer each);
  CollectResult (^anyMatch)(StreamPredicate pred, bool *result);
  CollectResult (^allMatch)(StreamPredicate pred, bool *result);
  CollectResult (^noneMatch)(StreamPredicate pred, bool *result);

  /* abandon an unrun chain */
  void          (^close)(void);

  Stream        *s_;        /* private: the underlying stream */
  FStream       *pool_next_;/* private: thread-local pool link */
};

/**
 * Wrap a stream from stream.h in the fluent interface. Takes ownership of `s`.
 * Never returns NULL (see "Never NULL" above); StreamFluent(NULL) is the inert FStream.
 */
FStream *StreamFluent(Stream *s);

/* ---- chain starters: the stream.h sources, already wrapped. Never NULL. ---------------------- */
FStream *FluentOfArray(const void *base, size_t n, size_t stride);       /* items point at array elements   */
FStream *FluentOfPointers(const void *const *ptrs, size_t n);            /* items are the pointers          */
FStream *FluentOfBlocks(StreamSource source);                            /* items come from your blocks     */
FStream *FluentOfDescriptor(const CollectionDescriptorV2 *source, void *ctx);  /* reuse an existing descriptor */

#endif /* STREAM_FLUENT_H */
