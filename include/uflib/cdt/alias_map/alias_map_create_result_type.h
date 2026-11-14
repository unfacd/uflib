/**
 * @file alias_map_create_result_type.h
 * @brief AliasMapCreateResult — outcome of AliasMapCreateWithDescriptor().
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_CREATE_RESULT_TYPE_H
#define UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_CREATE_RESULT_TYPE_H

#include <stddef.h>

typedef enum {
    ALIASMAP_CREATE_OK = 0,
    ALIASMAP_CREATE_INVALID_ARGUMENT, ///< bad descriptor, or JSON that is valid but off-contract
    ALIASMAP_CREATE_INVALID_JSON,     ///< JSON syntax error
    ALIASMAP_CREATE_DUPLICATE_ALIAS,
    ALIASMAP_CREATE_NO_MEMORY,
} AliasMapCreateStatus;

typedef struct {
    AliasMapCreateStatus status;
    size_t               failed_collector;   ///< offending collector; SIZE_MAX when n/a
    size_t               failed_alias;       ///< offending alias; SIZE_MAX when n/a
    size_t               conflict_collector; ///< earlier occurrence of a duplicate; SIZE_MAX when n/a
    size_t               conflict_alias;     ///< earlier occurrence of a duplicate; SIZE_MAX when n/a
    size_t               json_offset;        ///< byte offset of a JSON failure; SIZE_MAX when n/a
    size_t               group_count;        ///< populated on ALIASMAP_CREATE_OK
    size_t               alias_count;        ///< populated on ALIASMAP_CREATE_OK
} AliasMapCreateResult;

#endif /* UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_CREATE_RESULT_TYPE_H */
