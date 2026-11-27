/**
 * @file id_64bit.h
 * @brief Opaque 64-bit identifier to pointer map, one instantiation of rh_template.h.
 *
 * The key is a bare identifier with no structure to exploit; the value is an
 * address whose meaning the consumer decides. The map never dereferences or
 * frees the value, so the consumer also keeps ownership. Identifier zero is an
 * ordinary key: the empty-slot marker lives in the value instead, which is why
 * a NULL pointer cannot be stored.
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

#ifndef UFLIB_ID_64BIT_H
#define UFLIB_ID_64BIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>

/** Opaque map handle; its layout is private to id_64bit.c. */
typedef struct Id64Map Id64Map;

typedef enum Id64Result {
  ID64_FULL = -2,     ///< a bounded map is at its entry limit
  ID64_ERROR = -1,    ///< allocation failure, or a NULL value
  ID64_INSERTED = 0,  ///< the identifier was absent and is now stored
  ID64_UPDATED = 1    ///< the identifier was present and its pointer was replaced
} Id64Result;

/**
 * @brief Create a map.
 *
 * @param max_entries Entry limit, or 0 for unbounded. A bounded map is sized for
 *                    exactly that many entries at creation and never reallocates.
 * @param seed        Hash seed; randomise it when identifiers are client chosen.
 *
 * @return A map, or NULL when allocation fails or the sizes overflow.
 */
PUBLIC_API Id64Map *Id64MapCreate(size_t max_entries, uint64_t seed);

/** @brief Release the map and its buckets. The values are untouched. */
PUBLIC_API void Id64MapDestroy(Id64Map *map);

/**
 * @brief Add or replace one entry.
 *
 * @param value Consumer-owned pointer; NULL is rejected because it marks an
 *              empty slot.
 *
 * @return ID64_INSERTED, ID64_UPDATED, or a negative Id64Result on failure —
 *         notably ID64_FULL for a bounded map at its limit.
 */
PUBLIC_API Id64Result Id64MapPut(Id64Map *map, uint64_t identifier, void *value);

/** @brief Value for @p identifier, or NULL when absent. */
PUBLIC_API void *Id64MapGet(const Id64Map *map, uint64_t identifier);

/** @brief Remove and return the value for @p identifier, or NULL when absent. */
PUBLIC_API void *Id64MapRemove(Id64Map *map, uint64_t identifier);

/**
 * @brief Iterate: start with *cursor = 0. No insert or erase while iterating.
 *
 * The order is bucket order, not sorted; the value is copied out, so nothing
 * points into the table. @return false at the end.
 */
PUBLIC_API bool Id64MapNext(const Id64Map *map, size_t *cursor, uint64_t *identifier, void **value);

/** @brief Number of stored entries. */
PUBLIC_API size_t Id64MapCount(const Id64Map *map);

#endif /* UFLIB_ID_64BIT_H */
