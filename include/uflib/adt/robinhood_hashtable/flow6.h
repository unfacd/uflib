/**
 * @file flow6.h
 * @brief IPv6 (source, destination) flow-tuple table, one instantiation of rh_template.h.
 *
 * The slot is the key inline followed by the value: 40 bytes, so a lookup follows
 * no pointer. Addresses are raw network-order bytes (struct in6_addr layout).
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

#ifndef UFLIB_FLOW6_H
#define UFLIB_FLOW6_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>

#include <uflib/adt/flow_table/flow_table_type.h>

/** Opaque table handle; its layout is private to flow6.c. */
typedef struct Flow6Map Flow6Table;

/** A flow tuple: the same 36-byte layout the flow table uses. Both ports zero
 * is the empty-slot marker and is rejected. */
typedef FlowTableKeyV6 Flow6Key;

typedef enum Flow6Result {
  FLOW6_FULL = -2,     ///< a bounded table is at its entry limit
  FLOW6_ERROR = -1,    ///< allocation failure, or the reserved key
  FLOW6_INSERTED = 0,  ///< the key was absent and is now stored
  FLOW6_UPDATED = 1    ///< the key was present and its value was replaced
} Flow6Result;

/**
 * @brief Create a table.
 *
 * @param initial_flows Expected entry count; 0 picks the minimum capacity.
 * @param max_flows     Entry limit, or 0 for unbounded. A bounded table never
 *                      reallocates once at that size.
 * @param seed          Hash seed; randomise it when keys are client chosen.
 */
PUBLIC_API Flow6Table *Flow6Create(size_t initial_flows, size_t max_flows, uint64_t seed);

/** @brief Release the table. NULL is safe. */
PUBLIC_API void Flow6Destroy(Flow6Table *table);

/** @brief Remove every entry, keeping the current capacity. */
PUBLIC_API void Flow6Clear(Flow6Table *table);

/** @brief Insert or replace one key. */
PUBLIC_API Flow6Result Flow6Insert(Flow6Table *table, const Flow6Key *key, uint32_t value);

/** @brief Look up one key; writes @p value only on a hit. */
PUBLIC_API bool Flow6Find(const Flow6Table *table, const Flow6Key *key, uint32_t *value);

/** @brief Erase one key. False when it was absent. */
PUBLIC_API bool Flow6Erase(Flow6Table *table, const Flow6Key *key);

/** @brief Batched lookup with prefetch pipelining; returns the hit count. */
PUBLIC_API size_t Flow6FindBatch(const Flow6Table *table, const Flow6Key *keys, size_t count, uint32_t *values,
                      bool *found);

/** @brief Stored entry count. */
PUBLIC_API size_t Flow6Size(const Flow6Table *table);

/** @brief Slot count, the identity of the current generation. */
PUBLIC_API size_t Flow6Capacity(const Flow6Table *table);

#endif /* UFLIB_FLOW6_H */
