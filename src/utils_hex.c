/**
 * @file utils_hex.c
 * @brief Hexadecimal encode/decode primitives: bin2hex, hexchr2bin, hex2bin.
 *
 * Copyright (C) 2015-2026 unfacd works
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

#include <uflib/standard_defs.h>
#include <uflib/utils_hex.h>

char *
bin2hex(const unsigned char * restrict bin, size_t len, char * restrict result_out)
{
  return bin2hex_case(bin, len, BIN2HEX_DEFAULT_LETTER_ENCODING, result_out);
}

char *
bin2hex_case(const unsigned char * restrict bin, size_t len, bool is_small_letter_hex, char * restrict result_out)
{
  char        *out;
  const char  *digits;
  size_t       i;

  if (bin == NULL || len == 0) {
    return NULL;
  }

  if (IS_PRESENT(result_out)) {
    out = result_out;
  } else {
    out = malloc(len * 2 + 1);
    if (out == NULL) {
      return NULL;
    }
  }

  digits = is_small_letter_hex ? "0123456789abcdef" : "0123456789ABCDEF";

  for (i = 0; i < len; i++) {
    out[i * 2]   = digits[bin[i] >> 4];
    out[i * 2 + 1] = digits[bin[i] & 0x0F];
  }

  out[len * 2] = '\0';

  return out;
}

int
hexchr2bin(const char hex, char *out)
{
  if (out == NULL){
    return 0;
  }

  if (hex >= '0' && hex <= '9') {
    *out = hex - '0';
  } else if (hex >= 'A' && hex <= 'F') {
    *out = hex - 'A' + 10;
  } else if (hex >= 'a' && hex <= 'f') {
    *out = hex - 'a' + 10;
  } else {
    return 0;
  }

  return 1;
}

size_t
hex2bin(const char *hex, unsigned char **out)
{
  size_t len;
  char   b1;
  char   b2;
  size_t i;

  if (hex == NULL || *hex == '\0' || out == NULL) {
    return 0;
  }

  len = strlen(hex);
  if (len % 2 != 0) {
    return 0;
  }

  len /= 2;

  // Validate every digit up front so an invalid input never allocates —
  // and therefore can never leak or leave *out dangling on failure.
  for (i = 0; i < len; i++) {
    if (!hexchr2bin(hex[i * 2], &b1) || !hexchr2bin(hex[i * 2 + 1], &b2)) {
      return 0;
    }
  }

  *out = malloc(len);
  if (*out == NULL) {
    return 0;
  }
  for (i=0; i<len; i++) {
    hexchr2bin(hex[i * 2], &b1);
    hexchr2bin(hex[i * 2 + 1], &b2);
    (*out)[i] = (b1 << 4) | b2;
  }
  return len;
}