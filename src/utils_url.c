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

#include <stddef.h>

#include <uflib/utils_urls.h>

void TokeniseUrlParams(char *str, UrlParamsDescriptor *tokens, size_t tokens_sz_hint)
{
  char *p;
  size_t counter = 0;
  UrlParamToken *param = tokens->tokens[counter];

  p = str;
  if (*p == '/') p++;
  param->token = p;

  while (1 != 2) {
    if (*p == '\0') {
      break;
    }
    if (*p == '\\' && *(p + 1) == '/') {
      p += 2;
      continue;
    }
    if (*p == '/') {
      *p = '\0';

      if (++counter == tokens_sz_hint) break;
      param = tokens->tokens[counter];
      param->token = p + 1;
    }

    p++;
  }

  tokens->tokens_sz = counter + 1;

}