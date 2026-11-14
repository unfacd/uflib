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
 * @brief URL helpers — path tokenisation and component percent-encoding.
 *
 */

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

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

/**
 * @brief Percent-encode a string as a single URI component (RFC 3986 §2.1).
 *
 * The unreserved set is recognised by explicit ranges rather than isalnum(), so
 * the result does not drift with the process locale.
 *
 * @param src     NUL-terminated input; NULL is treated as the empty string.
 * @param dst     Destination buffer, NUL-terminated on return.
 * @param dst_sz  Capacity of @p dst, terminator included.
 *
 * @return Bytes written excluding the terminator; -1 when the encoded form does
 *         not fit, leaving @p dst holding the truncated prefix.
 */
ssize_t
UrlEncodeComponent(const char *src, char *dst, size_t dst_sz)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t            written = 0;

  if (dst == NULL || dst_sz == 0) return -1;

  if (src != NULL) {
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
      const unsigned char c = *p;

      const bool unreserved = ((c >= 'A' && c <= 'Z') ||
                               (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9') ||
                               c == '-' || c == '.' || c == '_' || c == '~');

      if (unreserved) {
        if (written + 1 >= dst_sz) { dst[written] = '\0'; return -1; }
        dst[written++] = (char)c;
      } else {
        if (written + 3 >= dst_sz) { dst[written] = '\0'; return -1; }
        dst[written++] = '%';
        dst[written++] = hex[c >> 4];
        dst[written++] = hex[c & 0x0f];
      }
    }
  }

  dst[written] = '\0';

  return (ssize_t)written;
}
