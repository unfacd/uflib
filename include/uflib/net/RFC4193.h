//
// Created by ayman on 7/11/2026.
//

/**
 * @file RFC4193.h
 * @brief RFC4193
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 7/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef UFLIB_RFC4193_H
#define UFLIB_RFC4193_H

#include <stdbool.h>

#include "uflib_defs.h"

/* Address families */
typedef enum
{
  IP_FAMILY_UNKNOWN = 0,
  IP_FAMILY_IPV4    = 4,
  IP_FAMILY_IPV6    = 6
} NetIpFamilyEnum;

/* Input representation */
typedef enum
{
  IP_INPUT_BINARY = 0,
  IP_INPUT_TEXT   = 1
} NetIpFormatEnum;

PUBLIC_API bool IsIpAddressPrivate(NetIpFamilyEnum family, NetIpFormatEnum input, const void *address, bool *          is_private);

PUBLIC_API NetIpFamilyEnum IpAddressFamily(const char *address);

#endif //UFLIB_RFC4193_H
