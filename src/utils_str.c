/**
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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <regex.h>
#include <uflib/standard_defs.h>
#include <uflib/utils_str.h>

/**
 * @brief A simple unsigned long number literal string format verifier.
 * @param unsigned_long_str Lateral string representing an unsigned long number
 * @return true if literal is correctly formatted in conformance with unsigned long number
 */
bool
__attribute__((nonnull(1))) is_unsigned_long_format(const char *unsigned_long_str)
{
#define UNSIGNED_LONG_REGEX_PATTERN "^[1-9][[:digit:]]{1,19}$"
  regex_t regex;

  if (regcomp(&regex, UNSIGNED_LONG_REGEX_PATTERN, REG_EXTENDED) != 0) {
    return false;
  }

  int ret = regexec(&regex, unsigned_long_str, (size_t) 0, NULL, 0);
  regfree(&regex);

  if (ret == 0) {
    return true;
  }

  return false;

#undef UNSIGNED_LONG_REGEX_PATTERN
}

/**
 * @brief An implementation of strndup that does not check source length. Always allocates space as per given
 * size, plus extra 1 byte for null termination.
 * @param[in] s Source string to be copied
 * @param[in] n Size of buffer to allocate.
 * @return pointer to newly allocated and copied string
 */
char *
strbufdup(const char *s, size_t n)
{
  if (!IS_EMPTY(s)) {
    char *copy = malloc(n + 1);
    if (copy) {
      memcpy(copy, s, n);
      copy[n] = 0;
    }
    return copy;
  }

  return NULL;
}

/**
 * @brief Dynamically copy a provided string, with extra semantics around source string being null, in which case a
 * user supplied callback is executed, instead.
 * @param s source string to be copied
 * @param n max chars to copy from source string. Will only be used if source str length exceeded this value. Buffer allocated is always
 * extended by an additional char for null.
 * @param on_null user callback to execute, which must return a 'char *'
 * @return
 */
char *
strbufdup_nullable(const char *s, size_t n, char *(*on_null)(void))
{
  if (!IS_EMPTY(s)) {
    size_t copy_sz = strlen(s) > n ? n : strlen(s);
    char *copy = malloc(copy_sz + 1);
    if (copy) {
      memcpy(copy, s, copy_sz);
      copy[copy_sz] = 0;
    }
    return copy;
  } else return on_null();

}

/**
 * @brief A convenient front end that returns pointer to a user-allocated buffer upon string formatting.
 * Check it's usage with the macro STRINGIFY_PARAMETER. Also check mdsprintf().
 * @param user_allocated_buffer should be large enough for all params on the call
 * @param format specs printf-like
 * @return
 */
__attribute__ ((format (printf, 2, 3))) char *
sprintf_provided_buffer(char *user_allocated_buffer, char *format, ...)
{
  if (IS_EMPTY(user_allocated_buffer)) return NULL;

  va_list args;
  va_start(args, format);
  vsprintf(user_allocated_buffer, format, args);
  va_end(args);

  return user_allocated_buffer;
}

unsigned digits_count(uint64_t number, unsigned base)
{
	unsigned digits = 1;
	uint64_t power  = 1;

	while (number/power >= base) {
		++digits;
		power *= base;
	}

	return digits;
}

/**
 * unsigned long to ascii
 * user must allocate char *ptr and pass it in, esnsuring it is of correct size
 * NOTE: Note very well tested for boundaries
 */
char *ultoa(unsigned long value, char *ptr, int base)
{
  unsigned long t = 0,
  							res = 0;
  unsigned long tmp = value;
  int count = 0;

  if (NULL == ptr) {
    return NULL;
  }

  if (tmp == 0) {
    count++;
  }

  while	(tmp > 0) {
    tmp = tmp/base;
    count++;
  }

  ptr += count;

  *ptr = '\0';

  do {
    res = value - base * (t = value / base);
    if (res < 10) {
      * -- ptr = '0' + res;
    } else if ((res >= 10) && (res < 16)) {
        * --ptr = 'A' - 10 + res;
    }
  } while ((value = t) != 0);

  return (ptr);

}

#define FLOAT_PRECISION 4

/**
 * C++ version 0.4 char* style "itoa":
 * Written by Lukás Chmela
 * Released under GPLv3.
 */
/*char* itoa(int value, char* result, int base) {
  // check that the base if valid
  if (base < 2 || base > 36) { *result = '\0'; return result; }

  char* ptr = result, *ptr1 = result, tmp_char;
  int tmp_value;

  do {
    tmp_value = value;
    value /= base;
    *ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz" [35 + (tmp_value - value * base)];
  } while ( value );

  // Apply negative sign
  if (tmp_value < 0) *ptr++ = '-';
  *ptr-- = '\0';
  while(ptr1 < ptr) {
    tmp_char = *ptr;
    *ptr--= *ptr1;
    *ptr1++ = tmp_char;
  }
  return result;
}*/

int itoa(char *ptr, uint32_t number)
{
	char *origin = ptr;
	int size;

	do {
		*ptr++ = '0' + (number % 10);
		number /= 10;
	} while (number);

	size = ptr - origin;
	ptr--;

	while (origin < ptr) {
		char t = *ptr;
		*ptr-- = *origin;
		*origin++ = t;
	}

	return size;
}

int ftoa(char *outbuf, float f)
{
	uint64_t mantissa, int_part, frac_part;
	int safe_shift;
	uint64_t safe_mask;
	short exp2;
	char *p;

	union {
		int L;
		float F;
	} x;

	x.F = f;
	p = outbuf;

	exp2 = (unsigned char)(x.L >> 23) - 127;
	mantissa = (x.L & 0xFFFFFF) | 0x800000;
	frac_part = 0;
	int_part = 0;

	if (x.L < 0) {
		*p++ = '-';
	}

	if (exp2 < -36) {
		*p++ = '0';
		goto END;
	}

	safe_shift = -(exp2 + 1);
	safe_mask = 0xFFFFFFFFFFFFFFFFULL >>(64 - 24 - safe_shift);

	if (exp2 >= 64) {
		int_part = ULONG_MAX;
	} else if (exp2 >= 23) {
		int_part = mantissa << (exp2 - 23);
	} else if (exp2 >= 0) {
		int_part = mantissa >> (23 - exp2);
		frac_part = (mantissa) & safe_mask;
	} else /* if (exp2 < 0) */ {
		frac_part = (mantissa & 0xFFFFFF);
	}

	if (int_part == 0) {
		*p++ = '0';
	} else {
		p += itoa(p, int_part);
	}

	if (frac_part != 0) {
		int m;

		*p++ = '.';

		for (m = 0; m < FLOAT_PRECISION; m++) {
			frac_part = (frac_part << 3) + (frac_part << 1);
			*p++ = (frac_part >> (24 + safe_shift)) + '0';
			frac_part &= safe_mask;
		}

		for (; p[-1] == '0'; --p) {}

		if (p[-1] == '.') {
			--p;
		}
	}

END:
	*p = 0;
	return p - outbuf;
}

const char *traverse_quoted(const char *ptr)
{
  char quote;

  quote = *ptr;
  ptr++;
  while ((*ptr != quote) && (*ptr != '\0'))
  {
    //handle quoted chars
    if ((*ptr=='\\') && (*(ptr+1) != '\0')) ptr++;
    ptr++;
  }
  return ptr;
}

//https://github.com/attractivechaos/klib/blob/master/kstring.c
//TokenAux aux;
//for (p = TokeniseString("ab:cde:fg/hij::k", ":/", &aux); p; p = TokeniseString(0, 0, &aux)) {
//		kputsn(p, aux.p - p, s);
//	}

char *TokeniseString(const char *str, const char *sep_in, TokenAux *aux)
{
  const unsigned char *p, *start, *sep = (unsigned char *) sep_in;
  if (sep) { // set up the table
    if (str == 0 && aux->finished) return 0; // no need to set up if we have finished
    aux->finished = 0;
    if (sep[0] && sep[1]) {
      aux->sep = -1;
      aux->tab[0] = aux->tab[1] = aux->tab[2] = aux->tab[3] = 0;
      for (p = sep; *p; ++p) aux->tab[*p>>6] |= 1ull<<(*p&0x3f);
    } else aux->sep = sep[0];
  }
  if (aux->finished) return 0;
  else if (str) start = (unsigned char *) str, aux->finished = 0;
  else start = (unsigned char *) aux->p + 1;
  if (aux->sep < 0) {
    for (p = start; *p; ++p)
      if (aux->tab[*p>>6]>>(*p&0x3f)&1) break;
  } else {
    for (p = start; *p; ++p)
      if (*p == aux->sep) break;
  }
  aux->p = (const char *) p; // end of token
  if (*p == 0) aux->finished = 1; // no more tokens
  return (char*)start;
}

#ifndef HAVE_STRLCPY
size_t
mstrlcpy(char *dest, const char *src, size_t size)
{
  size_t ret = strlen(src);

  if(size)
  {
    size_t len = (ret >= size) ? size - 1 : ret;
    memcpy(dest, src, len);
    dest[len] = '\0';
  }
  return ret;
}
#else
size_t
mstrlcpy(char *dest, const char *src, size_t size)
{
	return strlcpy(dest, src, size);
}
#endif

char *mystrdup(const char *s)
{
  size_t len = 1+strlen(s);
  char *p = malloc(len);

  return p ? memcpy(p, s, len) : NULL;
}

/* The three helpers below decode UTF-8.  Without the capability they do not
   exist: the declarations stay in the header, which ships and cannot read a
   build's macro, so the absence is at the symbol. */
#if UFLIB_CAPABILITY_UTF8PROC

#include <utf8proc.h>

/**
 * defensive UTF-8 string length counter
 *
 * This function safely counts the number of Unicode characters (codepoints)
 * in a UTF-8 string with bounds checking and error handling.
 *  English / Latin text (ASCII): Exactly 255 characters (since 1 character = 1 byte).
 *  Special Alphabets (Cyrillic, Greek, Hebrew): Roughly 127 characters (since 1 character = 2 bytes).
 *  Asian Alphabets (Chinese, Japanese, Korean): Roughly 85 characters (since 1 character = 3 bytes).
 *  Emojis: Only 63 characters (since 1 character = 4 bytes).
 *
 *  If passed an empty string the function will return 0 and 'is_valid' will be true.
 *
 * @param str           Input UTF-8 string (must be NUL-terminated)
 * @param max_bytes     Maximum bytes to read from the string
 * @param bytes_read    Output: actual bytes consumed (can be NULL)
 * @param is_valid      Output: true if string is valid UTF-8 up to the read point (can be NULL)
 * @return              Number of characters counted. if 0 is returned, check the value of 'is_valid'
 */
size_t
__attribute__((nonnull(1))) DefensiveStrlenUtf8(const char *str, size_t max_bytes, size_t *bytes_read, bool *is_valid)
{
  if (str == NULL || max_bytes == 0 ) {
    if (bytes_read) *bytes_read = 0;
    if (is_valid) *is_valid = false;

    return 0;
  }

  const uint8_t *s = (const uint8_t *)str;
  size_t char_count = 0;
  size_t byte_pos = 0;
  bool valid_so_far = true;

  while (byte_pos < max_bytes) {
    int32_t codepoint;
    ssize_t seq_len;

    seq_len = utf8proc_iterate(s + byte_pos, max_bytes - byte_pos, &codepoint);

    if (seq_len > 0) {
      if (codepoint == 0) break;

      char_count++;
      byte_pos += seq_len;
    } else  { // seq_len < 0 Invalid UTF-8 sequence encountered
      valid_so_far = false;

      // Handle specific error codes for better diagnostics
      if (seq_len == UTF8PROC_ERROR_INVALIDUTF8) {
        // Invalid byte sequence - skip the invalid byte. Count as a replacement character (U+FFFD) per Unicode  practices
        char_count++;
        byte_pos++;
        continue;
      } else if (seq_len == UTF8PROC_ERROR_OVERFLOW) {
        // Incomplete sequence at buffer boundary - stop here. This is a boundary condition, not a hard error
        break;
      } else {
        // Other errors
        if (bytes_read) *bytes_read = byte_pos;
        if (is_valid) *is_valid = false;

        return char_count;
      }
    }
  }

  if (bytes_read) {
    *bytes_read = byte_pos;
  }
  if (is_valid) {
    *is_valid = valid_so_far;
  }

  return char_count;
}

/**
 * Zero-copy version that works directly on binary / byte buffers.
 *
 * @param buf           UTF-8 byte buffer
 * @param buf_size      Size of the buffer in bytes
 * @param max_chars     Maximum characters to count
 * @return              Number of characters counted
 */
size_t
DefensiveStrlenUtf8Binary(const uint8_t *buf, size_t buf_size, size_t max_chars)
{
  if (buf == NULL || buf_size == 0 || max_chars == 0) {
    return 0;
  }

  const uint8_t *s = buf;
  size_t char_count = 0;
  size_t byte_pos = 0;

  while (byte_pos < buf_size && char_count < max_chars) {
    int32_t codepoint;
    ssize_t seq_len = utf8proc_iterate(s + byte_pos, buf_size - byte_pos, &codepoint);

    if (seq_len <= 0) {
      break; // NUL, invalid sequence, or incomplete - stop
    }

    char_count++;
    byte_pos += seq_len;
  }

  return char_count;
}

/**
 * Validates a UTF-8 string without counting characters.
 *
 * @param str           Input UTF-8 string
 * @param max_bytes     Maximum bytes to validate
 * @return              true if the string is valid UTF-8
 */
bool
IsUtf8Valid(const char *str, size_t max_bytes)
{
  if (str == NULL || max_bytes == 0) {
    return false;
  }

  const uint8_t *s = (const uint8_t *)str;
  size_t pos = 0;

  while (pos < max_bytes) {
    int32_t codepoint;
    ssize_t seq_len = utf8proc_iterate(s + pos, max_bytes - pos, &codepoint);

    if (seq_len == 0) {
      // NUL terminator - valid end of string
      return true;
    }

    if (seq_len < 0) {
      // Invalid UTF-8 sequence
      return false;
    }

    pos += seq_len;
  }

  return true;
}

#endif /* UFLIB_CAPABILITY_UTF8PROC */

/**
 * Defensively measures ASCII string length with a hard maximum limit.
 *
 * @param str       Input string
 * @param max_sz   Maximum length to allow/scan
 * @param out_len   [out] Receives the actual length found:
 *                  - If valid: real length (<='max_sz')
 *                  - If too long: exactly 'max_sz' (string was truncated or unterminated)
 * @return true if string is properly null-terminated within max_sz, false otherwise
 */
bool
__attribute__((nonnull(1))) DefensiveStrlen(const char *str, size_t max_sz, size_t *out_len)
{
  if (IS_PRESENT(out_len)) {
    if (max_sz == 0)  {
      *out_len = 0;
      return false;
    }

    size_t len = 0;

    while (len < max_sz) {
      if (str[len] == '\0') {
        *out_len = len;
        return true;
      }

      len++;
    }

    // Reached max_sz without finding null terminator
    *out_len = max_sz; // max we scanned
    return false;   // String is too long or not null-terminated
  } else {
    size_t len = 0;

    while (len < max_sz) {
      if (str[len] == '\0') {
        return true;
      }

      len++;
    }

    // Reached max_sz without finding null terminator
    return false;   // String is too long or not null-terminated
  }
}

/**
 * Defensively measures ASCII string length with minimum and maximum limits.
 *
 * @param str       Input string
 * @param min_sz   Minimum allowed length (inclusive)
 * @param max_sz   Maximum allowed length (inclusive)
 * @param out_len   [out] Receives the actual length found:
 *                  - If valid: real length (between min_sz and max_sz)
 *                  - If invalid: length up to max_sz or 0
 * @return true if string meets all criteria (length between min_sz and max_sz, properly null-terminated, ASCII), false otherwise.
 */
bool
__attribute__((nonnull(1))) DefensiveStrlenWithMinMax(const char *str, size_t min_sz, size_t max_sz, size_t *out_len)
{
  if (IS_PRESENT(out_len)) {
    if (max_sz == 0 || min_sz > max_sz) {
      *out_len = 0;
      return false;
    }

    size_t len = 0;

    while (len < max_sz) {
      if (str[len] == '\0') {
        *out_len = len;

        if (len >= min_sz) {
          return true;
        } else {
          return false;
        }
      }

      len++;
    }

    // Reached max_sz without null terminator
    *out_len = max_sz;
    return false;
  } else {
    if (max_sz == 0 || min_sz > max_sz) {
      return false;
    }

    size_t len = 0;

    while (len < max_sz) {
      if (str[len] == '\0') {
        if (len >= min_sz) {
          return true;
        } else {
          return false;
        }
      }

      len++;
    }

    // Reached max_sz without null terminator
    return false;
  }
}