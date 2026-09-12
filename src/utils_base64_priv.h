/**
 * @file utils_base64_priv.h
 * @brief Shared private base64 encode/decode core.
 *
 * A single, parameterised implementation of the RFC 4648 base64 algorithms is
 * shared by the standard-alphabet module (utils_base64.c) and the URL-safe
 * module (utils_base64url.c).  The two alphabets differ only in characters 62
 * and 63 ('+'/'/' vs '-'/'_') and in whether '=' padding is emitted, so both
 * are described by a UflibBase64Alphabet and driven by one pair of core
 * routines.
 *
 * THIS FILE IS NOT INSTALLED.  It is private to the library implementation.
 * Consumers must never include this file.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_UTILS_BASE64_PRIV_H
#define UFLIB_UTILS_BASE64_PRIV_H

#include <stddef.h>

/*!
 * A base64 alphabet: the 64-character encoding table (no terminator), the
 * padding character (or '\0' when the alphabet is unpadded), and a 256-entry
 * reverse table mapping each input byte to its 6-bit value, or -1 when the
 * byte is not part of the alphabet.
 */
typedef struct {
    const char  *enc64;   ///< 64-char encode alphabet, index 0..63.
    char         pad;     ///< Padding char, or '\0' for unpadded alphabets.
    const short *rev;     ///< 256-entry reverse table, -1 = invalid byte.
} UflibBase64Alphabet;

/* Alphabet descriptors (defined in utils_base64.c). */
extern const UflibBase64Alphabet kUflibBase64Std;  ///< RFC 4648: '+', '/', padded.
extern const UflibBase64Alphabet kUflibBase64Url;  ///< RFC 4648 §5: '-', '_', unpadded.

/*!
 * Shared decoder.  Decodes up to `length` bytes of `str` into the caller-sized
 * `out`, NUL-terminating it and writing the decoded count to `*ret`.  Returns 1
 * on success, 0 on invalid input.
 *
 * When `lenient` is set: whitespace is skipped, the URL-safe characters '-'
 * and '_' are accepted in place of '+' and '/', missing padding is allowed,
 * and over-padding/trailing garbage are ignored (this mode assumes the
 * standard alphabet).
 *
 * When `nul_terminated` is set, a NUL byte ends the input early (the C-string
 * contract of the standard base64 API).  When clear, a NUL is an ordinary byte
 * and is rejected by the alphabet — required by the `(pointer, length)` API so
 * an embedded NUL is a hard error rather than silent truncation.
 */
int sBase64DecodeCore(const unsigned char *str, int length, unsigned char *out,
                      int *ret, const UflibBase64Alphabet *abc, int lenient,
                      int nul_terminated);

/*!
 * Shared encoder.  Encodes `len` bytes of `data` into caller-sized `out`,
 * NUL-terminating it.  Returns the number of encoded characters (excluding
 * the terminator).  Padding is emitted only when the alphabet has a pad char.
 */
size_t sBase64EncodeCore(const unsigned char *data, size_t len, unsigned char *out,
                         const UflibBase64Alphabet *abc);

#endif /* UFLIB_UTILS_BASE64_PRIV_H */
