/**
 * @file alias_map_descriptor_type.h
 * @brief AliasMapDescriptor — bulk-construction input for AliasMapCreateWithDescriptor().
 *
 * The descriptor and every string it points at are read only during the call;
 * the map copies what it keeps, so the descriptor may live on the stack and be
 * discarded as soon as the constructor returns.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_DESCRIPTOR_TYPE_H
#define UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_DESCRIPTOR_TYPE_H

#include <stddef.h>

#include <uflib/cdt/alias_map/alias_map_collector_type.h>
#include <uflib/cdt/alias_map/alias_map_create_result_type.h>
#include <uflib/logger/logger_type.h>

typedef struct {
    const AliasMapCollector *collectors_ptr;  ///< read-only; may be NULL when collector_count is 0
    size_t                   collector_count; ///< number of groups to load; 0 builds an empty map
    UfLogger                *logger_ptr;      ///< caller-owned; may be NULL for a silent map
    AliasMapCreateResult     result;          ///< output only; always written
} AliasMapDescriptor;

#endif /* UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_DESCRIPTOR_TYPE_H */
