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
#include <stdlib.h>
#include <uflib/utils_base64.h>

static const char base64_table[] =
        {
                'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
                'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
                'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
                'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
                '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/', '\0'
        };

static const char base64_pad = '=';

static const short base64_reverse_table[256] =
        {
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
                52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
                -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
                -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
                41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
        };

/**
 * Provided a base size, return the final buffer size needed to accommodate b64 encoding operation on a buffer of that size
 * The returned size includes extra byte for '\0'
 * @param str_sz base buffer size, normally output of strlen
 * @return b64 size adjusted buffer allocation size
 */
__attribute__((pure)) size_t  GetBase64BufferAllocationSize (size_t str_sz)
{
  return (((str_sz + 2) / 3) * 5)+1;//+1 for null
}

/**
 * Base64 encoding for binary memory buffers.
 * @param buffer The binary buffer to be encoded
 * @param length length of binary buffer to be encoded
 * @param str_in user-supplied storage buffer. If provided, encoded buffer will be saved into that, otherwise the function
 * will allocate necessary storage space. Use GetBase64BufferAllocationSize() to estimate storage size for the buffer.
 *
 * @return pointer to encoded buffer
 * @dynamic_memory: ALLOCATES 'char *' which the user must free (only if str_provided was omitted)
 */
unsigned char *base64_encode(const unsigned char *buffer, int length, unsigned char *str_provided)
{
  const unsigned char *current = buffer;
  unsigned char *p;
  unsigned char *result;

  if ((length + 2) < 0 || ((length + 2) / 3) >= (1 << (sizeof(int) * 8 - 2))) {
    return NULL;
  }

  if (IS_PRESENT(str_provided))	result = str_provided;
  else	result = malloc(((length + 2) / 3) * 5);

  p = result;

  while(length > 2) {
    *p++ = base64_table[current[0] >> 2];
    *p++ = base64_table[((current[0] & 0x03) << 4) + (current[1] >> 4)];
    *p++ = base64_table[((current[1] & 0x0f) << 2) + (current[2] >> 6)];
    *p++ = base64_table[current[2] & 0x3f];

    current += 3;
    length -= 3;
  }

  if (length != 0) {
    *p++ = base64_table[current[0] >> 2];
    if (length > 1) {
      *p++ = base64_table[((current[0] & 0x03) << 4) + (current[1] >> 4)];
      *p++ = base64_table[(current[1] & 0x0f) << 2];
      *p++ = base64_pad;
    } else {
      *p++ = base64_table[(current[0] & 0x03) << 4];
      *p++ = base64_pad;
      *p++ = base64_pad;
    }
  }

  *p = '\0';
  return result;
}

/**
 * Decodes a b64 encoded char string into original, mostly binary, format
 * @param str A char string encoded with b64
 * @param length of char string requiring decoding
 * @param ret Size of the binary decoded buffer
 * @return pointer to a buffer containing final decoded binary buffer
 * @dynamic_memory: allocates char * which the user must free
 */
unsigned char *base64_decode(const unsigned char *str, int length, int *ret)
{
  const unsigned char *current = str;
  int ch, i = 0, j = 0, k;
  unsigned char *result = NULL;

  result = malloc(length + 1); //todo: this allocates more than actual size needed for original binary buffer

  while ((ch = *current++) != '\0' && length-- > 0) {
    if (ch == base64_pad)	break;

    ch = base64_reverse_table[ch];
    if (ch < 0)	continue;

    switch (i % 4) {
      case 0:
        result[j] = ch << 2;
        break;
      case 1:
        result[j++] |= ch >> 4;
        result[j] = (ch & 0x0f) << 4;
        break;
      case 2:
        result[j++] |= ch >> 2;
        result[j] = (ch & 0x03) << 6;
        break;
      case 3:
        result[j++] |= ch;
        break;
    }

    i++;
  }

  k = j;

  if (ch == base64_pad) {
    switch (i % 4) {
      case 1:
        free(result);
        return NULL;
      case 2:
        k++;
      case 3:
        result[k++] = 0;
    }
  }

  result[j] = '\0';
  *ret = j;

  return result;
}

unsigned char *base64_decode_buffered(const unsigned char *str, int length, unsigned char *decoded_in, int *ret)
{
  const unsigned char *current = str;
  int ch, i = 0, j = 0, k;
  unsigned char *result = NULL;

  if (IS_EMPTY(decoded_in))  result = calloc(1, length + 1); //todo: this allocates more than actual size needed for original binary buffer
  else result = decoded_in;

  while ((ch = *current++) != '\0' && length-- > 0) {
    if (ch == base64_pad)	break;

    ch = base64_reverse_table[ch];
    if (ch < 0)	continue;

    switch (i % 4) {
      case 0:
        result[j] = ch << 2;
        break;
      case 1:
        result[j++] |= ch >> 4;
        result[j] = (ch & 0x0f) << 4;
        break;
      case 2:
        result[j++] |= ch >> 2;
        result[j] = (ch & 0x03) << 6;
        break;
      case 3:
        result[j++] |= ch;
        break;
    }

    i++;
  }

  k = j;

  if (ch == base64_pad) {
    switch (i % 4) {
      case 1:
        free(result);
        return NULL;
      case 2:
        k++;
      case 3:
        result[k++] = 0;
    }
  }

  result[j] = '\0';
  *ret = j;

  return result;
}
