/**
 * Copyright (C) 2015-2021 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UFSRV_UTILS_BASE64_H
#define UFSRV_UTILS_BASE64_H

#include <stddef.h>

size_t  GetBase64BufferAllocationSize (size_t str_sz);
unsigned char *base64_encode(const unsigned char *, int, unsigned char *str_in);
unsigned char *base64_decode(const unsigned char *, int, int *);
unsigned char *base64_decode_buffered(const unsigned char *str, int length, unsigned char *decoded_in, int *ret);

#endif //UFSRV_UTILS_BASE64_H
