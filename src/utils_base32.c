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


#include <stdint.h>
#include <math.h>
#include <uflib/base32.h>

/* Douglas Crockford's Base32 alphabet: 0-9 then A-Z, excluding I, L, O and U. */
static const char base32_encoding[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* Unpadded encoded length (excluding the NUL terminator) for n input bytes. */
#define BASE32_ENC_LEN(n) (((n) * 8 + 4) / 5)
/* Decoded byte count for n encoded characters. */
#define BASE32_DEC_LEN(n) (((n) * 5) / 8)

size_t base32enc(char *dest, const void *_src, size_t ssize)
{
  const unsigned char *src = _src;
  uint32_t buffer = 0;
  int bits = 0;
  size_t dk = 0;

  if (!dest || (!src && ssize > 0))
    return 0;

  if (ssize == 0) {
    dest[0] = 0;
    return 0;
  }

  for (size_t i = 0; i < ssize; i++) {
    buffer = (buffer << 8) | src[i];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      dest[dk++] = base32_encoding[(buffer >> bits) & 0x1F];
    }
  }

  if (bits > 0)
    dest[dk++] = base32_encoding[(buffer << (5 - bits)) & 0x1F];

  dest[dk] = 0;
  return dk;
}

/* Pure (reentrant) decoder: derive the 5-bit value by arithmetic rather than a
 * 256-entry lookup table, so the module holds no static mutable state — no lazy
 * table init, hence no first-call data race.  For a 32-symbol alphabet the
 * table's speed advantage is negligible, so this is the right trade-off. */
static int base32_decode_char(char c, bool is_lenient)
{
  if (is_lenient) {
    /* Crockford ambiguity: I/L -> 1, O -> 0. */
    if (c == 'I' || c == 'i' || c == 'L' || c == 'l')
      return 1;
    if (c == 'O' || c == 'o')
      return 0;
    /* Case-insensitive: normalise lowercase to uppercase. */
    if (c >= 'a' && c <= 'z')
      c = (char)(c - 'a' + 'A');
  }

  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'H')
    return c - 'A' + 10;   /* no I */
  if (c >= 'J' && c <= 'K')
    return c - 'J' + 18;   /* no L */
  if (c >= 'M' && c <= 'N')
    return c - 'M' + 20;   /* no O */
  if (c >= 'P' && c <= 'T')
    return c - 'P' + 22;   /* no U */
  if (c >= 'V' && c <= 'Z')
    return c - 'V' + 27;

  return -1;
}

size_t base32dec_ex(void *dest, size_t dest_len, const char *src, uint32_t flags)
{
  bool is_lenient = (flags & BASE32DEC_FLAG_LENIENT) != 0;
  bool accept_padding = (flags & BASE32DEC_FLAG_ACCEPT_PADDING) != 0;
  bool partial_on_error = (flags & BASE32DEC_FLAG_PARTIAL_ON_ERR) != 0;

  uint8_t *out = dest;
  size_t out_idx = 0;
  uint32_t buffer = 0;
  int bits = 0;

  if (!dest || !src)
    return 0;

  for (const char *p = src; *p != '\0'; p++) {
    char c = *p;
    int val;

    if (c == '-' && is_lenient)
      continue;
    if (c == '=' && accept_padding)
      break;

    val = base32_decode_char(c, is_lenient);
    if (val < 0) {
      if (partial_on_error)
        break;               /* return bytes decoded so far */
      return (size_t)-1;     /* invalid character */
    }

    buffer = (buffer << 5) | (uint32_t)val;
    bits += 5;

    while (bits >= 8) {
      bits -= 8;
      if (out_idx >= dest_len)
        return out_idx;   /* destination full: truncate */
      out[out_idx++] = (uint8_t)((buffer >> bits) & 0xFF);
    }
  }

  return out_idx;
}

size_t base32dec(void *dest, size_t dest_len, const char *src, bool is_lenient)
{
  return base32dec_ex(dest, dest_len, src,
                      is_lenient ? (BASE32DEC_FLAG_LENIENT | BASE32DEC_FLAG_ACCEPT_PADDING) : 0);
}

size_t base32encsize(size_t count)
{
  return BASE32_ENC_LEN(count);
}

size_t base32decsize(size_t count)
{
  return BASE32_DEC_LEN(count);
}

/**
 * @brief Provide estimated encoded buffer size for
 * @param src_sz byte size of input buffer
 * @return byte size output buffer, not including terminating null char
 */
size_t
Base32ProvideEncodedBufferSize(size_t src_sz)
{
  return (size_t)+floor(ceil(src_sz / 5.f) * 8); //how many chunks of 5 bytes we have, and ceil because 0.1 chunk is still 1 chunk. a chunk is made of 8 characters
  //return ((src_sz * 8) + 4) / 5; //this formula undersizes
}
