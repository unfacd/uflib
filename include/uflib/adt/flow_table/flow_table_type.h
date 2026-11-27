/**
 * @file flow_table_type.h
 * @brief Opaque flat flow table handle and its key value types.
 *
 * A Robin Hood open-addressing table keyed by a flow tuple. Two key families
 * share one implementation: IPv4 keys are 4+4 address bytes, IPv6 keys are
 * 16+16, and the slot is sized for the family the table was created with, so
 * the IPv4 hot path keeps its compact 16-byte slots.
 *
 * The layout, hashing and probing live in the implementation; consumers see
 * only this handle and pass it back to the FlowTable* functions.
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

#ifndef UFLIB_FLOW_TABLE_TYPE_H
#define UFLIB_FLOW_TABLE_TYPE_H

#include <stdint.h>

/** Opaque table handle. Large, growable and heap-owned; never embedded. */
typedef struct FlowTable FlowTable;

/** Address family of the keys a table stores. Fixed at creation. */
typedef enum FlowTableKind {
  FLOW_TABLE_KIND_V4 = 0, ///< 4-byte source and destination addresses
  FLOW_TABLE_KIND_V6 = 1  ///< 16-byte source and destination addresses
} FlowTableKind;

/**
 * IPv4 flow tuple.
 *
 * A key whose two ports are both zero is reserved: the table uses it as the
 * empty-slot marker and rejects it, just as it rejects a key whose addresses
 * are all zero. Source and destination ports of a real flow are never zero.
 */
typedef struct FlowTableKeyV4 {
  uint32_t source_ip;      ///< any consistent byte order
  uint32_t destination_ip; ///< any consistent byte order
  uint16_t source_port;
  uint16_t destination_port;
} FlowTableKeyV4;

/** IPv6 flow tuple. Same reserved zero-port rule as the IPv4 key. */
typedef struct FlowTableKeyV6 {
  uint8_t  source[16];      ///< address bytes, in network order
  uint8_t  destination[16]; ///< address bytes, in network order
  uint16_t source_port;
  uint16_t destination_port;
} FlowTableKeyV6;

typedef enum FlowTableResult {
  FLOW_TABLE_ERROR = -1,   ///< rejected: NULL table, reserved key, wrong kind, or allocation failure
  FLOW_TABLE_INSERTED = 0, ///< key was absent and is now stored
  FLOW_TABLE_UPDATED = 1   ///< key was present and its value was replaced
} FlowTableResult;

#endif /* UFLIB_FLOW_TABLE_TYPE_H */
