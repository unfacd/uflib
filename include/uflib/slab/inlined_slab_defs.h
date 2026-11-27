/**
 * @file inlined_slab_defs.h
 * @brief Compile-time defaults and policy flags for InlinedSlab.
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

#ifndef UFLIB_INLINED_SLAB_DEFS_H
#define UFLIB_INLINED_SLAB_DEFS_H

/** Witness written by Init; every operation rejects a slab without it. */
#define INLINED_SLAB_MAGIC 0x534C4142u  /* "SLAB" */

/** Bytes a guard trailer occupies behind the link. */
#define INLINED_SLAB_GUARD_BYTES 4u

/** Minimum slot: one link and one pointer of payload. */
#define INLINED_SLAB_MIN_SLOT_SIZE (sizeof(void *) + sizeof(uint32_t))

/** Written into a taken slot's trailer; a different value means misuse. */
#define INLINED_SLAB_LIVE_MAGIC 0xA5A5A5A5u

/** Written into a freed slot's trailer; the guard's double-free witness. */
#define INLINED_SLAB_FREED_MAGIC 0x5A5A5A5Au

typedef enum InlinedSlabFlags {
  INLINED_SLAB_FLAG_NONE          = 0,       ///< no policy
  INLINED_SLAB_FLAG_ZERO_ON_TAKE  = 1u << 0, ///< memset every slot before it is returned
  INLINED_SLAB_FLAG_GUARD         = 1u << 1  ///< magic trailer; rejects double free and foreign puts
} InlinedSlabFlags;

#endif /* UFLIB_INLINED_SLAB_DEFS_H */
