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

#ifndef UFLIB_UTILS_URLS_H
#define UFLIB_UTILS_URLS_H

#include <uflib/uflib_defs.h>

#include <stddef.h>

/*! Single token in a tokenised URL path — points into the sacrificial string. */
typedef struct {
  char *token;  ///< Pointer to the start of this token within the path string
} UrlParamToken;

/*! Descriptor holding the parsed URL parameter tokens.
 *
 * The caller pre-allocates the @c tokens pointer array and each individual
 * @c UrlParamToken object.  After a successful call to @c TokeniseUrlParams,
 * @c tokens_sz holds the actual number of tokens emitted (1‑indexed — an
 * empty string produces a count of 1, not 0).
 */
typedef struct {
  UrlParamToken **tokens;   ///< Array of pointers to individual @c UrlParamToken objects
  size_t          tokens_sz; ///< Actual number of tokens populated (1‑indexed count)
} UrlParamsDescriptor;

PUBLIC_API void TokeniseUrlParams(char *str, UrlParamsDescriptor *tokens, size_t tokens_sz_hint);

#endif //UFSRV_UTILS_URLS_H
