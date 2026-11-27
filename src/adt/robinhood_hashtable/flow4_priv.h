/**
 * @file flow4_priv.h
 * @brief The IPv4 flow table instantiation, shared by flow4.c and flow_table.c.
 *
 * Private to the library: it defines the template parameters and the generated
 * table for the 12-byte tuple layout FlowTableKeyV4 already has, so the flow
 * table's V4 entry points can be static inline in their own unit and the module
 * wrappers in flow4.c wrap the very same instantiation rather than a copy of the
 * algorithm. Include it at most once per translation unit.
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

#ifndef UFLIB_ADT_FLOW4_PRIV_H
#define UFLIB_ADT_FLOW4_PRIV_H

#include <uflib/adt/flow_table/flow_table_type.h>
#include <uflib/adt/robinhood_hashtable/rh_common.h>

static inline uint64_t
sFlow4Hash(const FlowTableKeyV4 *key, uint64_t secret_a, uint64_t secret_b)
{
  const uint64_t ports = ((uint64_t)key->source_port << 16) | key->destination_port;
  return sRhtFoldMultiply(sRhtLoad64(key) ^ secret_a, ports ^ secret_b);
}

static inline bool
sFlow4Equal(const FlowTableKeyV4 *left, const FlowTableKeyV4 *right)
{
  return sRhtLoad64(left) == sRhtLoad64(right) && left->source_port == right->source_port &&
         left->destination_port == right->destination_port;
}

#define RhtTemplateName Flow4Map
#define RhtTemplateKey FlowTableKeyV4
#define RhtTemplateValue uint32_t
#define RhtTemplateHash(key, secret_a, secret_b) sFlow4Hash((key), (secret_a), (secret_b))
#define RhtTemplateKeyEq(left, right) sFlow4Equal((left), (right))
#define RhtTemplateSlotEmpty(slot) ((slot)->key.source_port == 0 && (slot)->key.destination_port == 0)
#include <uflib/adt/robinhood_hashtable/rh_template.h>

_Static_assert(sizeof(FlowTableKeyV4) == 12, "the IPv4 key is two addresses and two ports");
_Static_assert(sizeof(Flow4MapSlot) == 16, "the IPv4 slot stays at 16 bytes");

#endif /* UFLIB_ADT_FLOW4_PRIV_H */
