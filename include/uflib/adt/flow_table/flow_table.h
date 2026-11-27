/**
 * @file flow_table.h
 * @brief Flat Robin Hood hash table for IPv4 and IPv6 flow-tuple lookups.
 *
 * The key is stored inline in the slot, so a lookup walks the bucket array
 * without following a pointer. Probe distance is recomputed from the resident
 * key's hash rather than stored. The slot is sized by the table's key family:
 * 16 bytes for IPv4, 48 for IPv6, so IPv4 keeps four slots per cache line while
 * IPv6 remains exact rather than hashed or truncated. The table doubles at 7/8
 * load and grows without bound.
 *
 * Constraints:
 *   - One family per table; the V4 functions reject a V6 table and vice versa.
 *   - A key whose two ports are both zero is reserved as the empty marker and
 *     is rejected.
 *   - One writer at a time. Concurrent readers are safe while no writer runs.
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

#ifndef UFLIB_FLOW_TABLE_H
#define UFLIB_FLOW_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uflib/uflib_defs.h>
#include <uflib/adt/flow_table/flow_table_type.h>

/**
 * @brief Create a table sized for @p initial_capacity entries of one family.
 *
 * @param kind             Key family the table accepts; fixed for its lifetime.
 * @param initial_capacity Expected entry count; 0 picks the minimum capacity.
 * @param seed             Hash seed; use a random value to resist flooding.
 *
 * @return A table, or NULL when allocation fails or the kind is unknown.
 */
PUBLIC_API FlowTable *FlowTableCreate(FlowTableKind kind, size_t initial_capacity, uint64_t seed);

/** @brief Key family the table was created for, or -1 for a NULL table. */
PUBLIC_API int FlowTableKindOf(const FlowTable *table);

/** @brief Release the table and its buckets. NULL is safe. */
PUBLIC_API void FlowTableDestroy(FlowTable *table);

/** @brief Remove every entry, keeping the current capacity. NULL is a no-op. */
PUBLIC_API void FlowTableClear(FlowTable *table);

/**
 * @brief Insert or replace one IPv4 key.
 *
 * @return FLOW_TABLE_ERROR for a NULL or non-IPv4 table, a reserved key, or when
 * a grow is required and cannot be allocated; otherwise INSERTED or UPDATED.
 */
PUBLIC_API FlowTableResult FlowTableInsertV4(FlowTable *table, const FlowTableKeyV4 *key, uint32_t value);

/** @brief Look up one IPv4 key. Writes @p value only on a hit. */
PUBLIC_API bool FlowTableFindV4(const FlowTable *table, const FlowTableKeyV4 *key, uint32_t *value);

/** @brief Erase one IPv4 key. False when the table, family or key does not match. */
PUBLIC_API bool FlowTableEraseV4(FlowTable *table, const FlowTableKeyV4 *key);

/**
 * @brief Resolve a batch of IPv4 keys: hash and prefetch first, probe second.
 *
 * @param keys   Keys to resolve.
 * @param count  Number of keys.
 * @param values Receives the value per hit; entries for misses are untouched.
 * @param found  Receives the per-key outcome.
 *
 * @return The number of hits.
 */
PUBLIC_API size_t FlowTableFindBatchV4(const FlowTable *table, const FlowTableKeyV4 *keys, size_t count,
                                       uint32_t *values, bool *found);

/** @brief Insert or replace one IPv6 key. Same result contract as the IPv4 form. */
PUBLIC_API FlowTableResult FlowTableInsertV6(FlowTable *table, const FlowTableKeyV6 *key, uint32_t value);

/** @brief Look up one IPv6 key. Writes @p value only on a hit. */
PUBLIC_API bool FlowTableFindV6(const FlowTable *table, const FlowTableKeyV6 *key, uint32_t *value);

/** @brief Erase one IPv6 key. False when the table, family or key does not match. */
PUBLIC_API bool FlowTableEraseV6(FlowTable *table, const FlowTableKeyV6 *key);

/** @brief Batch resolve for IPv6 keys; same contract as the IPv4 form. */
PUBLIC_API size_t FlowTableFindBatchV6(const FlowTable *table, const FlowTableKeyV6 *keys, size_t count,
                                       uint32_t *values, bool *found);

/** @brief Stored entry count. */
PUBLIC_API size_t FlowTableSize(const FlowTable *table);

/** @brief Bucket count; the identity of the current generation. */
PUBLIC_API size_t FlowTableCapacity(const FlowTable *table);

#endif /* UFLIB_FLOW_TABLE_H */
