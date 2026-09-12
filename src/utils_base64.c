/**
 * @file utils_base64.c
 * @brief Base64 (RFC 4648) encode/decode implementation.
 */

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
 *
 * From: ~/repos/1.ufsrv/uflib
  * clang -std=gnu17 -D_GNU_SOURCE -I include \
  src/utils_base64.c tests/utils/utils_base64_test_standalone.c \
  -o build/base64_standalone

  ./build/base64_standalone
 */

#include <uflib/standard_defs.h>
#include <stdlib.h>
#include <string.h>
#include <uflib/utils_base64.h>

#include "utils_base64_priv.h"

/* ── Standard alphabet (RFC 4648) ───────────────────────────────────────── */

static const char kUflibBase64StdEnc[64] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const short kUflibBase64StdRev[256] =
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

/* ── URL-safe alphabet (RFC 4648 §5) ────────────────────────────────────── */

static const char kUflibBase64UrlEnc[64] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static const short kUflibBase64UrlRev[256] =
        {
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1,
                52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
                -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63,
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

const UflibBase64Alphabet kUflibBase64Std = { kUflibBase64StdEnc, '=', kUflibBase64StdRev };
const UflibBase64Alphabet kUflibBase64Url  = { kUflibBase64UrlEnc, '\0', kUflibBase64UrlRev };

/**
 * Given a base size, return the final buffer size needed to accommodate b64 encoding operation on a buffer of that size
 * The returned size includes extra byte for '\0'
 * @param str_sz base buffer size, normally output of strlen
 * @return b64 size adjusted buffer allocation size
 */
__attribute__((pure)) size_t  GetBase64BufferAllocationSize(size_t str_sz)
{
  return (((str_sz + 2) / 3) * 5) + 1;//+1 for null
}

/**
 * Calculate the original input byte size from its b64 encoded format.
 * @param b64_encoded base 64 encoded character string. Must meet minimum size requirement for a b64 encoded string.
 * @return original input byte size or -1 on error
 */
ssize_t GetBase64BufferDecodedSize(const char *b64_encoded)
{
  unsigned int padding_char_sz = 0;
  const char *ep; //end pointer
  size_t len;

  if (b64_encoded == NULL) {
    return -1;
  }

  len = strlen(b64_encoded);

  // The shortest valid encoding is a single 4-character group ("AA==").
  // Reject anything shorter; the decoded-size formula assumes the length is a
  // multiple of 4.
  if (len < 4) {
    return -1;
  }

  for (ep = b64_encoded + len - 1; ep >= b64_encoded; ep--) {
    if (*ep != '=') break;
    ++padding_char_sz;
  }

  return (3 * (len / 4)) - padding_char_sz;
}

/**
 * Base64 encoding for binary memory buffers.
 * @param buffer[in] The binary buffer to be encoded
 * @param length length of binary buffer to be encoded
 * @param str_provided[inout] user-supplied storage buffer. If provided, encoded buffer will be saved into that, otherwise the function
 * will allocate necessary storage space. Use GetBase64BufferAllocationSize() to estimate storage size for the buffer.
 *
 * @return pointer to encoded buffer
 * @dynamic_memory: ALLOCATES 'char *' which the user must free (only if str_provided was omitted)
 */
size_t
sBase64EncodeCore(const unsigned char *data, size_t len, unsigned char *out,
                  const UflibBase64Alphabet *abc)
{
  const char *enc = abc->enc64;
  const char padc = abc->pad;
  unsigned char *p = out;

  while (len >= 3) {
    *p++ = (unsigned char)enc[data[0] >> 2];
    *p++ = (unsigned char)enc[((data[0] & 0x03) << 4) | (data[1] >> 4)];
    *p++ = (unsigned char)enc[((data[1] & 0x0f) << 2) | (data[2] >> 6)];
    *p++ = (unsigned char)enc[data[2] & 0x3f];
    data += 3;
    len -= 3;
  }

  if (len == 2) {
    *p++ = (unsigned char)enc[data[0] >> 2];
    *p++ = (unsigned char)enc[((data[0] & 0x03) << 4) | (data[1] >> 4)];
    *p++ = (unsigned char)enc[(data[1] & 0x0f) << 2];
    if (padc)
      *p++ = (unsigned char)padc;
  } else if (len == 1) {
    *p++ = (unsigned char)enc[data[0] >> 2];
    *p++ = (unsigned char)enc[(data[0] & 0x03) << 4];
    if (padc) {
      *p++ = (unsigned char)padc;
      *p++ = (unsigned char)padc;
    }
  }

  *p = '\0';
  return (size_t)(p - out);
}

unsigned char *base64_encode(const unsigned char *buffer, int length, unsigned char *str_provided)
{
  unsigned char *result;
  size_t alloc;

  if (buffer == NULL || length < 0) {
    return NULL;
  }

  // Encoded size: 4 chars per 3-byte group plus headroom, computed in size_t so
  // `length + 2` cannot overflow an int.  Reserve at least one byte so a
  // zero-length encode still has room for its NUL terminator.
  alloc = ((size_t)length + 2) / 3 * 5;
  if (alloc == 0) {
    alloc = 1;
  }

  if (IS_PRESENT(str_provided))	result = str_provided;
  else {
    result = malloc(alloc);
    if (result == NULL) {
      return NULL;
    }
  }

  (void)sBase64EncodeCore(buffer, (size_t)length, result, &kUflibBase64Std);
  return result;
}

/* Shared decoder — see utils_base64_priv.h for the full contract. */
int
sBase64DecodeCore(const unsigned char *str, int length, unsigned char *out,
                  int *ret, const UflibBase64Alphabet *abc, int lenient,
                  int nul_terminated)
{
  const unsigned char *current = str;
  const char padc = abc->pad;
  const short *rev = abc->rev;
  int ch, j = 0, nchars = 0, pad = 0, nbits = 0;
  unsigned int acc = 0;

  while (length-- > 0) {
    ch = *current++;
    if (ch == '\0' && nul_terminated) {
      break;   // C-string contract: NUL ends input
    }
    if (padc && ch == padc) {
      if (lenient) {
        break;   // ignore padding and any trailing garbage
      }
      pad = 1;
      while (length-- > 0 && *current == padc) {
        ++pad;
        ++current;
      }
      break;
    }

    if (lenient) {
      // Skip whitespace.
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f') {
        continue;
      }
      // Accept the URL-safe alphabet: '-' == '+', '_' == '/'.
      if (ch == '-') {
        ch = '+';
      } else if (ch == '_') {
        ch = '/';
      }
    }

    ch = rev[ch];
    if (ch < 0) {
      return 0;
    }

    // Accumulate 6-bit groups and emit a byte only once 8 bits are available,
    // so a partial final group never writes a spurious trailing byte.
    ++nchars;
    acc = (acc << 6) | (unsigned int)ch;
    nbits += 6;
    if (nbits >= 8) {
      nbits -= 8;
      out[j++] = (unsigned char)((acc >> nbits) & 0xFFu);
      acc &= (1u << nbits) - 1u;
    }
  }

  if (!lenient) {
    // Reject non-canonical input: '=' before any data, '=' after a complete
    // group, a pad count that does not complete the group to a multiple of 4,
    // or missing padding on a partial group.
    const int rem = nchars % 4;
    if (padc) {
      const int expected_pad = (rem == 0) ? 0 : (4 - rem);
      if (rem == 1 || pad != expected_pad) {
        return 0;
      }
    } else if (rem == 1) {
      // Unpadded alphabet: a lone trailing character cannot decode to a byte.
      return 0;
    }

    // Reject non-zero padding bits: the unused low bits of a partial final
    // group must be zero for a canonical encoding.
    if (nbits > 0 && acc != 0) {
      return 0;
    }
  }

  out[j] = '\0';
  *ret = j;
  return 1;
}

unsigned char *base64_decode(const unsigned char *str, int length, int *ret)
{
  unsigned char *result;

  if (str == NULL || ret == NULL || length < 0) {
    return NULL;
  }

  result = malloc((size_t)length + 1); //todo: this allocates more than actual size needed for original binary buffer
  if (result == NULL) {
    return NULL;
  }

  if (!sBase64DecodeCore(str, length, result, ret, &kUflibBase64Std, 0, 1)) {
    free(result);
    return NULL;
  }

  return result;
}

unsigned char *base64_decode_lenient(const unsigned char *str, int length, int *ret)
{
  unsigned char *result;

  if (str == NULL || ret == NULL || length < 0) {
    return NULL;
  }

  result = malloc((size_t)length + 1);
  if (result == NULL) {
    return NULL;
  }

  if (!sBase64DecodeCore(str, length, result, ret, &kUflibBase64Std, 1, 1)) {
    free(result);
    return NULL;
  }

  return result;
}

unsigned char *base64_decode_buffered(const unsigned char *str, int length, unsigned char *decoded_in, int *ret)
{
  unsigned char *result;
  int own = 0;

  if (str == NULL || ret == NULL || length < 0) {
    return NULL;
  }

  if (IS_EMPTY(decoded_in)) {
    result = calloc(1, (size_t)length + 1); //todo: this allocates more than actual size needed for original binary buffer
    if (result == NULL) {
      return NULL;
    }
    own = 1;
  } else {
    result = decoded_in;
  }

  if (!sBase64DecodeCore(str, length, result, ret, &kUflibBase64Std, 0, 1)) {
    // Only free when we allocated the buffer ourselves; never free the
    // caller's decoded_in buffer.
    if (own) {
      free(result);
    }
    return NULL;
  }

  return result;
}
