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

#include <uflib/standard_defs.h>
#include <uflib/utils_hex.h>

//based on https://nachtimwald.com/2017/09/24/hex-encode-and-decode-in-c/

char *bin2hex(const unsigned char * restrict bin, size_t len, char * restrict result_out)
{
  char   *out;
  size_t  i;

  if (bin == NULL || len == 0) {
    return NULL;
  }

  if (IS_PRESENT(result_out)) {
    out = result_out;
  } else {
    out = malloc(len*2+1);
  }

  for (i=0; i < len; i++) {
    out[i*2]   = "0123456789ABCDEF"[bin[i] >> 4];
    out[i*2+1] = "0123456789ABCDEF"[bin[i] & 0x0F];
  }

  out[len*2] = '\0';

  return out;
}

int hexchr2bin (const char hex, char *out)
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

size_t hex2bin(const char *hex, unsigned char **out)
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

  *out = malloc(len);
  memset(*out, 'A', len);
  for (i=0; i<len; i++) {
    if (!hexchr2bin(hex[i*2], &b1) || !hexchr2bin(hex[i*2+1], &b2)) {
      return 0;
    }
    (*out)[i] = (b1 << 4) | b2;
  }
  return len;
}