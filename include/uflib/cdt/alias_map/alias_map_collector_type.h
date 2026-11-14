/**
 * @file alias_map_collector_type.h
 * @brief AliasMapCollector — one value together with the aliases that reach it.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_COLLECTOR_TYPE_H
#define UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_COLLECTOR_TYPE_H

#include <stddef.h>

typedef struct {
    const char        *value_ptr;   ///< value shared by the group; copied at construction
    const char *const *aliases_ptr; ///< alias_count entries; copied at construction
    size_t             alias_count; ///< number of aliases; must be at least 1
} AliasMapCollector;

#endif /* UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_COLLECTOR_TYPE_H */
