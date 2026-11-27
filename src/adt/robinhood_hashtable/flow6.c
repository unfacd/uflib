/**
 * @file flow6.c
 * @brief The IPv6 flow table module: wrappers over the shared instantiation.
 *
 * flow6_priv.h owns the five template choices and the static inline table; this
 * file gives them an exported, NULL-tolerant API and pins the result-code
 * numbering. The same instantiation backs FlowTable's V6 entry points.
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

#include <uflib/adt/robinhood_hashtable/flow6.h>

#include <robinhood_hashtable/flow6_priv.h>

_Static_assert((int)FLOW6_FULL == (int)RH_TEMPLATE_FULL &&
                   (int)FLOW6_ERROR == (int)RH_TEMPLATE_ERROR &&
                   (int)FLOW6_INSERTED == (int)RH_TEMPLATE_INSERTED &&
                   (int)FLOW6_UPDATED == (int)RH_TEMPLATE_UPDATED,
               "the public result codes mirror the template's");

PUBLIC_API Flow6Table *
Flow6Create(size_t initial_flows, size_t max_flows, uint64_t seed)
{
  return sFlow6MapCreate(initial_flows, max_flows, seed);
}

PUBLIC_API void
Flow6Destroy(Flow6Table *table)
{
  sFlow6MapDestroy(table);
}

PUBLIC_API void
Flow6Clear(Flow6Table *table)
{
  if (table) sFlow6MapClear(table);
}

PUBLIC_API Flow6Result
Flow6Insert(Flow6Table *table, const Flow6Key *key, uint32_t value)
{
  if (!table || !key) return FLOW6_ERROR;
  return (Flow6Result)sFlow6MapInsert(table, key, value);
}

PUBLIC_API bool
Flow6Find(const Flow6Table *table, const Flow6Key *key, uint32_t *value)
{
  return table && key && sFlow6MapFind(table, key, value);
}

PUBLIC_API bool
Flow6Erase(Flow6Table *table, const Flow6Key *key)
{
  return table && key && sFlow6MapErase(table, key);
}

PUBLIC_API size_t
Flow6FindBatch(const Flow6Table *table, const Flow6Key *keys, size_t count, uint32_t *values,
               bool *found)
{
  if (!table || !keys || !values || !found) return 0;
  return sFlow6MapFindBatch(table, keys, count, values, found);
}

PUBLIC_API size_t
Flow6Size(const Flow6Table *table)
{
  return table ? sFlow6MapSize(table) : 0;
}

PUBLIC_API size_t
Flow6Capacity(const Flow6Table *table)
{
  return table ? sFlow6MapCapacity(table) : 0;
}
