/**
 * @file inlined_slab.h
 * @brief Public API for the embeddable, single-owner InlinedSlab.
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

#ifndef UFLIB_INLINED_SLAB_H
#define UFLIB_INLINED_SLAB_H

#include <stdbool.h>
#include <stddef.h>

#include <uflib/uflib_defs.h>
#include <uflib/slab/inlined_slab_defs.h>
#include <uflib/slab/inlined_slab_type.h>

/**
 * @brief Bytes a region must provide for the descriptor's geometry.
 *
 * Returns 0 when the geometry is invalid: capacity 0, slot_size below the
 * minimum, alignment not a power of two, or arithmetic overflow.
 */
PUBLIC_API size_t InlinedSlabBytes(const InlinedSlabDescriptor *descriptor);

/**
 * @brief Thread a free list through the descriptor's region.
 *
 * @param slab       Caller-owned storage, zeroed by this call.
 * @param descriptor Geometry and placement. @c link_offset must be a multiple
 *                   of 4 with room for the guard trailer when
 *                   INLINED_SLAB_FLAG_GUARD is set.
 *
 * @return 0 on success, -1 on any invalid argument.
 *
 * The pool never allocates: it only writes links. A non-zero return leaves the
 * slab zeroed and unusable.
 *
 * @code{.c}
 * InlinedSlabDescriptor descriptor = {
 *   .region = region, .region_bytes = region_bytes, .capacity = 64,
 *   .slot_size = sizeof(Slot), .alignment = _Alignof(Slot), .link_offset = 0,
 * };
 * if (InlinedSlabInit(&slab, &descriptor) != 0) fail();
 * @endcode
 */
PUBLIC_API int InlinedSlabInit(InlinedSlab *slab, const InlinedSlabDescriptor *descriptor);

/** @brief Rebuild the free list. Fails (-1) while any slot is leased. */
PUBLIC_API int InlinedSlabReset(InlinedSlab *slab);

/** @brief Pop one slot, or NULL when the pool is exhausted. O(1), no lock. */
PUBLIC_API void *InlinedSlabTake(InlinedSlab *slab);

/** @brief Return one slot. False for a pointer this pool does not own, or a double free under guard. */
PUBLIC_API bool InlinedSlabPut(InlinedSlab *slab, void *slot);

/** @brief Pop up to @p wanted slots; returns how many were written. */
PUBLIC_API size_t InlinedSlabTakeBulk(InlinedSlab *slab, void **slots, size_t wanted);

/** @brief Return up to @p count slots; returns how many were accepted. */
PUBLIC_API size_t InlinedSlabPutBulk(InlinedSlab *slab, void *const *slots, size_t count);

/** @brief Slots available to take. */
PUBLIC_API size_t InlinedSlabAvailable(const InlinedSlab *slab);

/** @brief Slots the pool was built with. */
PUBLIC_API size_t InlinedSlabCapacity(const InlinedSlab *slab);

/** @brief Largest number of simultaneously leased slots since init or reset. */
PUBLIC_API size_t InlinedSlabHighWater(const InlinedSlab *slab);

/** @brief True when @c low_water is set and availability has fallen below it. */
PUBLIC_API bool InlinedSlabScarce(const InlinedSlab *slab);

#endif /* UFLIB_INLINED_SLAB_H */
