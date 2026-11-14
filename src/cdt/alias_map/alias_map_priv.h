/**
 * @file alias_map_priv.h
 * @brief AliasMap — internal interface shared by the module's translation units.
 *
 * Never installed; colocated with the implementation.  These helpers exist only
 * so the JSON descriptor parser reports results and binds logging the same way
 * the struct descriptor constructor does.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_PRIV_H
#define UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_PRIV_H

#include <stddef.h>

#include <uflib/cdt/alias_map/alias_map.h>

#include "alias_map_type_priv.h"

const char *AliasMapCreateStatusName(AliasMapCreateStatus status);

void AliasMapResultFill(AliasMapCreateResult *result_ptr, AliasMapCreateStatus status,
                        size_t failed_collector, size_t failed_alias,
                        size_t conflict_collector, size_t conflict_alias,
                        size_t group_count, size_t alias_count);

void AliasMapLogBind(AliasMap *map_ptr, UfLogger *logger_ptr);

AliasMap *AliasMapCreateInternal(AliasMapDescriptor *descriptor_ptr, AliasMapOrigin origin);

#endif /* UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_PRIV_H */
