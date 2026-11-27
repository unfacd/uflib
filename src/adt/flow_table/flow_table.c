/**
 * @file flow_table.c
 * @brief The flow table: two rh_template.h instantiations behind one API.
 *
 * The family is fixed when the table is created and every entry point takes one
 * stored pointer, so the kind is tested once per call and never inside a probe.
 * Everything a probe does lives in the instantiation, which is static inline in
 * this translation unit: there is no second dispatch, no runtime-length address
 * compare and no per-slot branch.
 *
 * A key whose two ports are both zero is reserved as the empty-slot marker by
 * both instantiations, so a portless flow (ICMP, ESP) cannot be stored. Adding a
 * protocol field to the key is the way to lift that, not widening the ports.
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

#include <stdlib.h>

#include <uflib/adt/flow_table/flow_table.h>

#include <robinhood_hashtable/flow4_priv.h>
#include <robinhood_hashtable/flow6_priv.h>

/** The handle embeds the instantiation itself: the family tag plus one table
 * header, so a probe dereferences the slot array without an intermediate
 * pointer. The union keeps the handle at the larger of the two headers. */
struct FlowTable {
  FlowTableKind kind;
  union {
    Flow4Map v4;
    Flow6Map v6;
  } as;
};

/** Map an instantiation result onto the public enum. FULL cannot occur: the public
 * constructor is unbounded, but mapping it to ERROR keeps the two code spaces from
 * ever aliasing. */
static inline FlowTableResult
sResult(int result)
{
  switch (result) {
  case RH_TEMPLATE_INSERTED:
    return FLOW_TABLE_INSERTED;
  case RH_TEMPLATE_UPDATED:
    return FLOW_TABLE_UPDATED;
  default:
    return FLOW_TABLE_ERROR;
  }
}

PUBLIC_API FlowTable *
FlowTableCreate(FlowTableKind kind, size_t initial_capacity, uint64_t seed)
{
  if (kind != FLOW_TABLE_KIND_V4 && kind != FLOW_TABLE_KIND_V6) return NULL;

  FlowTable *table = calloc(1, sizeof(*table));
  if (!table) return NULL;
  table->kind = kind;
  /* max_entries 0: the public API has no bounded form and never returns FULL. */
  const int initialised = kind == FLOW_TABLE_KIND_V4
                              ? sFlow4MapInit(&table->as.v4, initial_capacity, 0, seed)
                              : sFlow6MapInit(&table->as.v6, initial_capacity, 0, seed);
  if (initialised != 0) {
    free(table);
    return NULL;
  }
  return table;
}

PUBLIC_API int
FlowTableKindOf(const FlowTable *table)
{
  return table ? (int)table->kind : -1;
}

PUBLIC_API void
FlowTableDestroy(FlowTable *table)
{
  if (!table) return;
  if (table->kind == FLOW_TABLE_KIND_V4) {
    sFlow4MapFini(&table->as.v4);
  } else {
    sFlow6MapFini(&table->as.v6);
  }
  free(table);
}

PUBLIC_API void
FlowTableClear(FlowTable *table)
{
  if (!table) return;
  if (table->kind == FLOW_TABLE_KIND_V4) {
    sFlow4MapClear(&table->as.v4);
  } else {
    sFlow6MapClear(&table->as.v6);
  }
}

PUBLIC_API FlowTableResult
FlowTableInsertV4(FlowTable *table, const FlowTableKeyV4 *key, uint32_t value)
{
  if (!table || !key || table->kind != FLOW_TABLE_KIND_V4) return FLOW_TABLE_ERROR;
  return sResult(sFlow4MapInsert(&table->as.v4, key, value));
}

PUBLIC_API bool
FlowTableFindV4(const FlowTable *table, const FlowTableKeyV4 *key, uint32_t *value)
{
  if (!table || !key || table->kind != FLOW_TABLE_KIND_V4) return false;
  return sFlow4MapFind(&table->as.v4, key, value);
}

PUBLIC_API bool
FlowTableEraseV4(FlowTable *table, const FlowTableKeyV4 *key)
{
  if (!table || !key || table->kind != FLOW_TABLE_KIND_V4) return false;
  return sFlow4MapErase(&table->as.v4, key);
}

PUBLIC_API size_t
FlowTableFindBatchV4(const FlowTable *table, const FlowTableKeyV4 *keys, size_t count,
                     uint32_t *values, bool *found)
{
  if (!table || !keys || !values || !found || table->kind != FLOW_TABLE_KIND_V4) return 0;
  return sFlow4MapFindBatch(&table->as.v4, keys, count, values, found);
}

PUBLIC_API FlowTableResult
FlowTableInsertV6(FlowTable *table, const FlowTableKeyV6 *key, uint32_t value)
{
  if (!table || !key || table->kind != FLOW_TABLE_KIND_V6) return FLOW_TABLE_ERROR;
  return sResult(sFlow6MapInsert(&table->as.v6, key, value));
}

PUBLIC_API bool
FlowTableFindV6(const FlowTable *table, const FlowTableKeyV6 *key, uint32_t *value)
{
  if (!table || !key || table->kind != FLOW_TABLE_KIND_V6) return false;
  return sFlow6MapFind(&table->as.v6, key, value);
}

PUBLIC_API bool
FlowTableEraseV6(FlowTable *table, const FlowTableKeyV6 *key)
{
  if (!table || !key || table->kind != FLOW_TABLE_KIND_V6) return false;
  return sFlow6MapErase(&table->as.v6, key);
}

PUBLIC_API size_t
FlowTableFindBatchV6(const FlowTable *table, const FlowTableKeyV6 *keys, size_t count,
                     uint32_t *values, bool *found)
{
  if (!table || !keys || !values || !found || table->kind != FLOW_TABLE_KIND_V6) return 0;
  return sFlow6MapFindBatch(&table->as.v6, keys, count, values, found);
}

PUBLIC_API size_t
FlowTableSize(const FlowTable *table)
{
  if (!table) return 0;
  return table->kind == FLOW_TABLE_KIND_V4 ? sFlow4MapSize(&table->as.v4) : sFlow6MapSize(&table->as.v6);
}

PUBLIC_API size_t
FlowTableCapacity(const FlowTable *table)
{
  if (!table) return 0;
  return table->kind == FLOW_TABLE_KIND_V4 ? sFlow4MapCapacity(&table->as.v4)
                                           : sFlow6MapCapacity(&table->as.v6);
}
