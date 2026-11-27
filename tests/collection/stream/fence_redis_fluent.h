/**
 * @file fence_redis_fluent.h
 * @brief Fence member uids as a fluent stream (clang blocks).
 *
 * Independent of the DescriptorV2 loader in fence_redis_backend.c: it shares no code or state
 * with it, so both can live in the same library.
 *
 *   CollectionResult   out;
 *   StreamCollectOpts  opts = FenceMemberEntryOpts(true);          // copy strings into an arena
 *
 *   CollectResult rc = FenceMemberUids(fid)
 *       ->filter(^bool(const void *p) { return ((const FenceMemberEntry *)p)->uid != requesting_uid; })
 *       ->limit(50)
 *       ->collect(opts, &out);
 *
 *   if (rc == COLLECT_OK) {
 *     const FenceMemberEntry *entries = out.items;
 *     for (size_t i = 0; i < out.count; i++) use(entries[i].uid, entries[i].raw);
 *     StreamResultFree(&opts, &out);
 *   }
 *
 *   size_t n;      FenceMemberUids(fid)->filter(isActive)->count(&n);            // copies nothing
 *   bool   found;  FenceMemberUids(fid)->anyMatch(^bool(const void *p) {         // stops at the first hit
 *                    return ((const FenceMemberEntry *)p)->uid == wanted; }, &found);
 *
 * Nothing touches the backend until the terminal call (collect/count/anyMatch/...).
 */
#ifndef FENCE_REDIS_FLUENT_H
#define FENCE_REDIS_FLUENT_H

#include <stdbool.h>

#include <uflib/collection/stream/stream_fluent.h>

typedef struct FenceMemberEntry {
  unsigned long  uid;
  char          *raw;     /* the Redis string. BORROWED while the chain runs (your blocks see it that
                             way); OWNED by the result once collect() has copied it */
} FenceMemberEntry;

/**
 * A chain starter: the member uids of fence `fid`.
 *
 * The source reads the Redis reply and a built-in stage validates and parses it, so the blocks you
 * chain only ever see well-formed members: null elements, non-numeric strings, trailing junk,
 * leading signs and uid 0 are already dropped. Items are FenceMemberEntry.
 */
FStream *FenceMemberUids(unsigned long fid);

/**
 * Options for ->collect() on a FenceMemberUids chain, returned BY VALUE so it can be written inline.
 * use_arena: strings are copied into the result's arena (one free for everything).
 * otherwise: each string is malloc'd and freed per element.
 * Release the result with StreamResultFree(&opts, &out) using the SAME options.
 */
StreamCollectOpts FenceMemberEntryOpts(bool use_arena);

#endif /* FENCE_REDIS_FLUENT_H */
