/**
 * @file flow6_priv.h
 * @brief The IPv6 flow table instantiation, shared by flow6.c and flow_table.c.
 *
 * Private to the library: the 36-byte tuple layout FlowTableKeyV6 already has,
 * instantiated once per including translation unit. Include it at most once per
 * unit.
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

#ifndef UFLIB_ADT_FLOW6_PRIV_H
#define UFLIB_ADT_FLOW6_PRIV_H

#include <uflib/adt/flow_table/flow_table_type.h>
#include <uflib/adt/robinhood_hashtable/rh_common.h>

static inline uint64_t
sFlow6Hash(const FlowTableKeyV6 *key, uint64_t secret_a, uint64_t secret_b)
{
  const uint64_t ports = ((uint64_t)key->source_port << 16) | key->destination_port;
  uint64_t mixed = sRhtFoldMultiply(sRhtLoad64(key->source) ^ secret_a,
                                    sRhtLoad64(key->source + 8) ^ secret_b);
  mixed = sRhtFoldMultiply(mixed, sRhtLoad64(key->destination) ^ secret_a);
  mixed = sRhtFoldMultiply(mixed, sRhtLoad64(key->destination + 8) ^ secret_b);
  return sRhtFoldMultiply(mixed ^ ports, ports ^ secret_a ^ secret_b);
}

static inline bool
sFlow6Equal(const FlowTableKeyV6 *left, const FlowTableKeyV6 *right)
{
  return ((sRhtLoad64(left->source) ^ sRhtLoad64(right->source)) |
          (sRhtLoad64(left->source + 8) ^ sRhtLoad64(right->source + 8)) |
          (sRhtLoad64(left->destination) ^ sRhtLoad64(right->destination)) |
          (sRhtLoad64(left->destination + 8) ^ sRhtLoad64(right->destination + 8)) |
          (uint64_t)(left->source_port ^ right->source_port) |
          (uint64_t)(left->destination_port ^ right->destination_port)) == 0;
}

#define RhtTemplateName Flow6Map
#define RhtTemplateKey FlowTableKeyV6
#define RhtTemplateValue uint32_t
#define RhtTemplateHash(key, secret_a, secret_b) sFlow6Hash((key), (secret_a), (secret_b))
#define RhtTemplateKeyEq(left, right) sFlow6Equal((left), (right))
#define RhtTemplateSlotEmpty(slot) ((slot)->key.source_port == 0 && (slot)->key.destination_port == 0)
#include <uflib/adt/robinhood_hashtable/rh_template.h>

_Static_assert(sizeof(FlowTableKeyV6) == 36, "the IPv6 key is two address bytes and two ports");
_Static_assert(sizeof(Flow6MapSlot) == 40, "the IPv6 slot is the key plus the value");

#endif /* UFLIB_ADT_FLOW6_PRIV_H */
