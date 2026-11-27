/**
 * @file inlined_slab_type.h
 * @brief Embeddable fixed-size slot pool with the free link inlined in the slot.
 *
 * The struct is meant to live inside its owner (a shard, a worker, a queue
 * object); the region it manages is caller-supplied, so the pool adds no
 * allocation of its own. Free slots are threaded through a uint32_t link stored
 * at @c link_offset inside each slot, and the head is an index, not a pointer.
 *
 * Every field is written only by this module. A consumer that zeroes, copies
 * or hand-edits the control block is caught by the magic witness on the next
 * operation instead of corrupting the free list.
 *
 * @b Thread-safety: single owner. Take/Put run on one thread at a time; the
 * pool carries no lock and no atomic. Cross-thread hand-off is the caller's
 * problem (ownership transfer through a queue), not the pool's.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_INLINED_SLAB_TYPE_H
#define UFLIB_INLINED_SLAB_TYPE_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Everything Init needs to build one pool.
 *
 * Describes both the geometry (slot size, capacity, alignment, link offset) and
 * the placement (region and its size), so a caller fills one struct instead of
 * threading eight arguments through the call.
 */
typedef struct InlinedSlabDescriptor {
  void *  region;       ///< slot memory; caller-owned, @c alignment-aligned
  size_t  region_bytes; ///< bytes available at @c region
  size_t  capacity;     ///< slots to build
  size_t  slot_size;    ///< bytes per slot, at least INLINED_SLAB_MIN_SLOT_SIZE
  size_t  alignment;    ///< slot alignment, a power of two
  size_t  link_offset;  ///< byte offset of the uint32_t link inside a slot
  size_t  low_water;    ///< advisory scarcity threshold; 0 disables
  unsigned flags;       ///< INLINED_SLAB_FLAG_*
} InlinedSlabDescriptor;

typedef struct InlinedSlab {
  unsigned char *region;      ///< first slot; caller-owned, @c alignment-aligned
  size_t         slot_size;   ///< stride between slots, a multiple of alignment
  size_t         capacity;    ///< slots in the region
  size_t         leased;      ///< slots currently taken
  size_t         high_water;  ///< largest @c leased observed
  size_t         low_water;   ///< advisory scarcity threshold; 0 disables
  size_t         link_offset; ///< byte offset of the uint32_t link inside a slot
  uint32_t       free_head;   ///< slot index + 1; 0 means exhausted
  unsigned       flags;       ///< INLINED_SLAB_FLAG_*
  uint32_t       magic;       ///< INLINED_SLAB_MAGIC once initialised
} InlinedSlab;

#endif /* UFLIB_INLINED_SLAB_TYPE_H */
