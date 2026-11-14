/**
 * @file alias_map_json_descriptor_type.h
 * @brief AliasMapJsonDescriptor — bulk construction from a JSON document.
 *
 * The JSON document is the same shape the configuration file uses for one
 * registry entry, collected into an array:
 *
 * @code{.json}
 * [
 *   {"value": "/opt/etc/ufsrv/assets/greeting.txt", "keys": ["greeting", "hello"]},
 *   {"value": "/opt/etc/ufsrv/assets/motd.txt",     "keys": ["motd"]}
 * ]
 * @endcode
 *
 * The contract is strict: the document must be an array of objects; every
 * object must carry exactly one `value` string and one non-empty `keys` array
 * of strings; unknown or repeated fields are rejected; nothing but whitespace
 * may follow the closing bracket.  Every alias and value is copied, so the
 * document may be released as soon as the constructor returns.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_JSON_DESCRIPTOR_TYPE_H
#define UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_JSON_DESCRIPTOR_TYPE_H

#include <uflib/cdt/alias_map/alias_map_create_result_type.h>
#include <uflib/logger/logger_type.h>

typedef struct {
    const char          *json_ptr;   ///< NUL-terminated JSON document; never retained
    UfLogger            *logger_ptr; ///< caller-owned; may be NULL for a silent map
    AliasMapCreateResult result;     ///< output only; always written
} AliasMapJsonDescriptor;

#endif /* UFLIB_CDT_ALIAS_MAP_ALIAS_MAP_JSON_DESCRIPTOR_TYPE_H */
