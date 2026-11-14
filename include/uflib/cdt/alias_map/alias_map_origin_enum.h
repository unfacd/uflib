/**
 * @file alias_map_origin_enum.h
 * @brief AliasMapOrigin — how a map was instantiated.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_ORIGIN_ENUM_H
#define UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_ORIGIN_ENUM_H

typedef enum {
    ALIASMAP_ORIGIN_NONE = 0,        ///< NULL map, or no origin recorded
    ALIASMAP_ORIGIN_INCREMENTAL = 1, ///< created empty, populated with mutators
    ALIASMAP_ORIGIN_DESCRIPTOR = 2,  ///< AliasMapCreateWithDescriptor()
    ALIASMAP_ORIGIN_JSON = 3,        ///< AliasMapCreateWithJsonDescriptor()
} AliasMapOrigin;

#endif /* UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_ORIGIN_ENUM_H */
