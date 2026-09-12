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

/**
 * @file utils_url.c
 * @brief URL path tokeniser — splits a path on '/' delimiters with
 *        escape-sequence ('\/') awareness.
 *
 * The tokeniser operates in-place on a sacrificial string, replacing
 * delimiters with NUL bytes.  Trailing slashes are ignored; leading
 * slashes are skipped.  Consecutive slashes produce empty tokens.
 */

#include <stddef.h>

#include <uflib/utils_urls.h>

/**
 * @brief Tokenise a URL path in-place by splitting on '/' delimiters.
 *
 * The tokeniser understands the escaped-slash sequence '\/' and will skip
 * past it without splitting.  Leading '/' characters are skipped; trailing
 * '/' characters are ignored.  Consecutive '/' delimiters produce empty
 * tokens (e.g. "a//b" yields three tokens: "a", "", "b").
 *
 * @note  The input string @p str is sacrificial — delimiters are overwritten
 *        with NUL bytes to demarcate tokens.  Pass a copy if the original
 *        must be preserved.
 *
 * @param str             Sacrificial NUL-terminated path string to tokenise.
 * @param tokens          Pre-allocated descriptor whose @c tokens array holds
 *                        pointers to individual @c UrlParamToken objects.
 *                        The caller must populate both the pointer array and
 *                        each @c UrlParamToken before calling.
 * @param tokens_sz_hint  Maximum number of tokens to emit (typically the
 *                        number of pre-allocated @c UrlParamToken slots).
 *                        The tokeniser may produce fewer.
 */
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

      if (counter + 1 == tokens_sz_hint ||
          *(p + 1) == '\0') { //ignore case where path ends with a trailing '/'
        break;
      }
      param = tokens->tokens[++counter];
      param->token = p + 1;
    }

    p++;
  }

  tokens->tokens_sz = counter + 1; //return true index size (ie no 0-indexed)

}