/**
 * @file inlined_slab.c
 * @brief Embeddable fixed-size slot pool; free list threaded through the slots.
 *
 * Every operation is O(1) and allocation free: Take pops an index off the free
 * head, Put pushes it back. Slots are addressed by index (index + 1 is stored),
 * so a stale pointer can be range-checked without dereferencing it.
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <uflib/slab/inlined_slab.h>

static bool
sIsInitialised(const InlinedSlab *slab)
{
  return slab && slab->magic == INLINED_SLAB_MAGIC && slab->region && slab->capacity;
}

static bool
sIsPowerOfTwo(size_t value)
{
  return value && (value & (value - 1)) == 0;
}

static unsigned char *
sSlot(const InlinedSlab *slab, size_t index)
{
  return slab->region + index * slab->slot_size;
}

static uint32_t *
sLink(const InlinedSlab *slab, void *slot)
{
  return (uint32_t *)((unsigned char *)slot + slab->link_offset);
}

static uint32_t *
sTrailer(const InlinedSlab *slab, void *slot)
{
  return (uint32_t *)((unsigned char *)slot + slab->link_offset + sizeof(uint32_t));
}

static size_t
sStride(size_t slot_size, size_t alignment)
{
  return (slot_size + (alignment - 1)) & ~(alignment - 1);
}

size_t
InlinedSlabBytes(const InlinedSlabDescriptor *descriptor)
{
  if (!descriptor) return 0;
  if (!sIsPowerOfTwo(descriptor->alignment) || descriptor->slot_size < INLINED_SLAB_MIN_SLOT_SIZE ||
      descriptor->capacity == 0 || descriptor->capacity > UINT32_MAX - 1u ||
      descriptor->alignment > descriptor->slot_size)
    return 0;
  if (descriptor->slot_size > SIZE_MAX / descriptor->capacity) return 0;
  return sStride(descriptor->slot_size, descriptor->alignment) * descriptor->capacity;
}

int
InlinedSlabInit(InlinedSlab *slab, const InlinedSlabDescriptor *descriptor)
{
  if (!slab || !descriptor || !descriptor->region) return -1;
  const size_t bytes = InlinedSlabBytes(descriptor);
  if (!bytes || descriptor->region_bytes < bytes) return -1;
  if (((uintptr_t)descriptor->region % descriptor->alignment) != 0) return -1;
  if ((descriptor->link_offset % sizeof(uint32_t)) != 0 ||
      descriptor->link_offset + sizeof(uint32_t) > descriptor->slot_size)
    return -1;
  if ((descriptor->flags & INLINED_SLAB_FLAG_GUARD) &&
      descriptor->link_offset + sizeof(uint32_t) + INLINED_SLAB_GUARD_BYTES > descriptor->slot_size)
    return -1;

  memset(slab, 0, sizeof(*slab));
  slab->magic = INLINED_SLAB_MAGIC;
  slab->region = descriptor->region;
  slab->slot_size = sStride(descriptor->slot_size, descriptor->alignment);
  slab->capacity = descriptor->capacity;
  slab->link_offset = descriptor->link_offset;
  slab->flags = descriptor->flags;
  slab->low_water = descriptor->low_water;
  return InlinedSlabReset(slab);
}

int
InlinedSlabReset(InlinedSlab *slab)
{
  if (!sIsInitialised(slab) || slab->leased) return -1;

  slab->free_head = 0;
  slab->high_water = 0;
  /* Push from the end so the first Take returns slot 0: predictable for tests
   * and for a caller that maps slot indices to a static resource table. */
  for (size_t index = slab->capacity; index > 0; index--) {
    void *slot = sSlot(slab, index - 1);
    *sLink(slab, slot) = slab->free_head;
    if (slab->flags & INLINED_SLAB_FLAG_GUARD) *sTrailer(slab, slot) = INLINED_SLAB_FREED_MAGIC;
    slab->free_head = (uint32_t)index;
  }
  slab->leased = 0;
  return 0;
}

void *
InlinedSlabTake(InlinedSlab *slab)
{
  if (!sIsInitialised(slab) || !slab->free_head) return NULL;

  const size_t index = (size_t)slab->free_head - 1u;
  void *slot = sSlot(slab, index);
  const uint32_t next = *sLink(slab, slot);

  if (slab->flags & INLINED_SLAB_FLAG_GUARD) {
    if (*sTrailer(slab, slot) != INLINED_SLAB_FREED_MAGIC) return NULL; /* corrupted list */
    *sTrailer(slab, slot) = INLINED_SLAB_LIVE_MAGIC;
  }

  slab->free_head = next;
  slab->leased++;
  if (slab->leased > slab->high_water) slab->high_water = slab->leased;
  if (slab->flags & INLINED_SLAB_FLAG_ZERO_ON_TAKE) memset(slot, 0, slab->slot_size);
  return slot;
}

bool
InlinedSlabPut(InlinedSlab *slab, void *slot)
{
  if (!sIsInitialised(slab) || !slot) return false;
  if ((unsigned char *)slot < slab->region) return false;

  const uintptr_t delta = (uintptr_t)((unsigned char *)slot - slab->region);
  if (delta % slab->slot_size) return false;
  const size_t index = (size_t)(delta / slab->slot_size);
  if (index >= slab->capacity || !slab->leased) return false;

  if (slab->flags & INLINED_SLAB_FLAG_GUARD) {
    uint32_t *trailer = sTrailer(slab, slot);
    if (*trailer != INLINED_SLAB_LIVE_MAGIC) return false; /* double free or foreign slot */
    *trailer = INLINED_SLAB_FREED_MAGIC;
  }

  *sLink(slab, slot) = slab->free_head;
  slab->free_head = (uint32_t)index + 1u;
  slab->leased--;
  return true;
}

size_t
InlinedSlabTakeBulk(InlinedSlab *slab, void **slots, size_t wanted)
{
  if (!sIsInitialised(slab) || !slots) return 0;
  size_t taken = 0;
  while (taken < wanted) {
    void *slot = InlinedSlabTake(slab);
    if (!slot) break;
    slots[taken++] = slot;
  }
  return taken;
}

size_t
InlinedSlabPutBulk(InlinedSlab *slab, void *const *slots, size_t count)
{
  if (!sIsInitialised(slab) || !slots) return 0;
  size_t returned = 0;
  for (size_t index = 0; index < count; index++) {
    if (!InlinedSlabPut(slab, slots[index])) break;
    returned++;
  }
  return returned;
}

size_t
InlinedSlabAvailable(const InlinedSlab *slab)
{
  if (!sIsInitialised(slab) || slab->leased > slab->capacity) return 0;
  return slab->capacity - slab->leased;
}

size_t
InlinedSlabCapacity(const InlinedSlab *slab)
{
  return sIsInitialised(slab) ? slab->capacity : 0;
}

size_t
InlinedSlabHighWater(const InlinedSlab *slab)
{
  return sIsInitialised(slab) ? slab->high_water : 0;
}

bool
InlinedSlabScarce(const InlinedSlab *slab)
{
  if (!sIsInitialised(slab) || !slab->low_water) return false;
  return InlinedSlabAvailable(slab) < slab->low_water;
}
