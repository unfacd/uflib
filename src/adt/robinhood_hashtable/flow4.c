/**
 * @file flow4.c
 * @brief The IPv4 flow table module: wrappers over the shared instantiation.
 *
 * flow4_priv.h owns the five template choices and the static inline table; this
 * file gives them an exported, NULL-tolerant API and pins the result-code
 * numbering. The same instantiation backs FlowTable's V4 entry points, so a
 * consumer that uses either spelling gets the one algorithm.
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

#include <uflib/adt/robinhood_hashtable/flow4.h>

#include <robinhood_hashtable/flow4_priv.h>

_Static_assert((int)FLOW4_FULL == (int)RH_TEMPLATE_FULL &&
                   (int)FLOW4_ERROR == (int)RH_TEMPLATE_ERROR &&
                   (int)FLOW4_INSERTED == (int)RH_TEMPLATE_INSERTED &&
                   (int)FLOW4_UPDATED == (int)RH_TEMPLATE_UPDATED,
               "the public result codes mirror the template's");

PUBLIC_API Flow4Table *
Flow4Create(size_t initial_flows, size_t max_flows, uint64_t seed)
{
  return sFlow4MapCreate(initial_flows, max_flows, seed);
}

PUBLIC_API void
Flow4Destroy(Flow4Table *table)
{
  sFlow4MapDestroy(table);
}

PUBLIC_API void
Flow4Clear(Flow4Table *table)
{
  if (table) sFlow4MapClear(table);
}

PUBLIC_API Flow4Result
Flow4Insert(Flow4Table *table, const Flow4Key *key, uint32_t value)
{
  if (!table || !key) return FLOW4_ERROR;
  return (Flow4Result)sFlow4MapInsert(table, key, value);
}

PUBLIC_API bool
Flow4Find(const Flow4Table *table, const Flow4Key *key, uint32_t *value)
{
  return table && key && sFlow4MapFind(table, key, value);
}

PUBLIC_API bool
Flow4Erase(Flow4Table *table, const Flow4Key *key)
{
  return table && key && sFlow4MapErase(table, key);
}

PUBLIC_API size_t
Flow4FindBatch(const Flow4Table *table, const Flow4Key *keys, size_t count, uint32_t *values,
               bool *found)
{
  if (!table || !keys || !values || !found) return 0;
  return sFlow4MapFindBatch(table, keys, count, values, found);
}

PUBLIC_API size_t
Flow4Size(const Flow4Table *table)
{
  return table ? sFlow4MapSize(table) : 0;
}

PUBLIC_API size_t
Flow4Capacity(const Flow4Table *table)
{
  return table ? sFlow4MapCapacity(table) : 0;
}
