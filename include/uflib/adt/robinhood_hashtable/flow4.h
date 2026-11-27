/**
 * @file flow4.h
 * @brief IPv4 (source, destination) flow-tuple table, one instantiation of rh_template.h.
 *
 * The key is laid out exactly as FlowTableKeyV4 is — two addresses then two
 * ports — so the 16-byte slot the flow table depends on is the slot this header
 * exposes, and a caller can pass its key struct without a copy pass.
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

#ifndef UFLIB_FLOW4_H
#define UFLIB_FLOW4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>

#include <uflib/adt/flow_table/flow_table_type.h>

/** Opaque table handle; its layout is private to flow4.c. */
typedef struct Flow4Map Flow4Table;

/** A flow tuple: the same 12-byte layout the flow table uses. Both ports zero is
 * the empty-slot marker and is rejected. */
typedef FlowTableKeyV4 Flow4Key;

typedef enum Flow4Result {
  FLOW4_FULL = -2,     ///< a bounded table is at its entry limit
  FLOW4_ERROR = -1,    ///< allocation failure, or the reserved key
  FLOW4_INSERTED = 0,  ///< the key was absent and is now stored
  FLOW4_UPDATED = 1    ///< the key was present and its value was replaced
} Flow4Result;

/**
 * @brief Create a table.
 *
 * @param initial_flows Expected entry count; 0 picks the minimum capacity.
 * @param max_flows     Entry limit, or 0 for unbounded. A bounded table never
 *                      reallocates once at that size.
 * @param seed          Hash seed; randomise it when keys are client chosen.
 */
PUBLIC_API Flow4Table *Flow4Create(size_t initial_flows, size_t max_flows, uint64_t seed);

/** @brief Release the table. NULL is safe. */
PUBLIC_API void Flow4Destroy(Flow4Table *table);

/** @brief Remove every entry, keeping the current capacity. */
PUBLIC_API void Flow4Clear(Flow4Table *table);

/** @brief Insert or replace one key. */
PUBLIC_API Flow4Result Flow4Insert(Flow4Table *table, const Flow4Key *key, uint32_t value);

/** @brief Look up one key; writes @p value only on a hit. */
PUBLIC_API bool Flow4Find(const Flow4Table *table, const Flow4Key *key, uint32_t *value);

/** @brief Erase one key. False when it was absent. */
PUBLIC_API bool Flow4Erase(Flow4Table *table, const Flow4Key *key);

/** @brief Batched lookup with prefetch pipelining; returns the hit count. */
PUBLIC_API size_t Flow4FindBatch(const Flow4Table *table, const Flow4Key *keys, size_t count, uint32_t *values,
                      bool *found);

/** @brief Stored entry count. */
PUBLIC_API size_t Flow4Size(const Flow4Table *table);

/** @brief Slot count, the identity of the current generation. */
PUBLIC_API size_t Flow4Capacity(const Flow4Table *table);

#endif /* UFLIB_FLOW4_H */
