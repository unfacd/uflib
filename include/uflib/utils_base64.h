/**
 * @file utils_base64.h
 * @brief Base64 (RFC 4648) encode/decode primitives and buffer-size helpers.
 */

/**
 * Copyright (C) 2015-2025 unfacd works
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

#ifndef UFLIB_UTILS_BASE64_H
#define UFLIB_UTILS_BASE64_H

#include <uflib/uflib_defs.h>

#include <stddef.h>
#include <sys/types.h>

/**
 * @brief Compute a safe upper bound for a base64-encoded output buffer.
 *
 * Returns a buffer size (in bytes) that is guaranteed to hold the NUL-terminated
 * base64 encoding of @p str_sz input bytes.  The value is deliberately
 * conservative: it reserves 5 output bytes per 3-byte input group (the exact
 * padded encoding occupies 4), plus one byte for the terminator, so callers may
 * hand the result straight to malloc() without further rounding.
 *
 * @param[in] str_sz  Number of input bytes to be encoded.
 *
 * @return Buffer size in bytes, always at least 1 (for the empty string's NUL).
 *
 * @code{.c}
 * size_t cap = GetBase64BufferAllocationSize(in_len);
 * unsigned char *out = malloc(cap);
 * if (out) base64_encode(in, in_len, out);
 * @endcode
 */
PUBLIC_API size_t  GetBase64BufferAllocationSize (size_t str_sz);

/**
 * @brief Calculate the original byte size from a base64-encoded string.
 *
 * Derives the decoded byte count as <tt>3 * (strlen / 4) - padding</tt>, i.e.
 * the number of bytes the NUL-terminated @p b64_encoded string decodes to.
 *
 * @param[in] b64_encoded  NUL-terminated, non-NULL base64 string (typically
 *                         padded).  Must be at least 4 characters long.
 *
 * @return The decoded byte count on success, or -1 if @p b64_encoded is NULL or
 *         shorter than the minimum accepted length (4 characters).
 *
 * @note The function assumes the length is a multiple of 4 (padded or a whole
 *       number of groups); unpadded partial groups under-report by one.
 *
 * @code{.c}
 * ssize_t n = GetBase64BufferDecodedSize("Zm9vYmFy");
 * // n == 6 — the decoded size of "foobar".
 * @endcode
 */
PUBLIC_API ssize_t GetBase64BufferDecodedSize(const char *b64_encoded);

/**
 * @brief Encode a binary buffer as a NUL-terminated base64 string.
 *
 * Renders @p length input bytes using the standard RFC 4648 alphabet
 * (A-Z, a-z, 0-9, +, /) with '=' padding, and always appends a NUL terminator.
 *
 * @param[in]     buffer        Input bytes to encode.  Must be non-NULL when
 *                              @p length > 0.
 * @param[in]     length        Number of bytes in @p buffer.  Must be >= 0.
 * @param[in,out] str_provided  Optional caller-provided output buffer.  When
 *                              non-NULL it receives the encoded text and must be
 *                              large enough (see GetBase64BufferAllocationSize()).
 *                              When NULL the result is malloc'd.
 *
 * @return Pointer to the NUL-terminated base64 text — equal to @p str_provided
 *         when supplied, otherwise a freshly malloc'd buffer the caller must
 *         free().  Returns NULL if @p buffer is NULL, @p length is negative, or
 *         the malloc'd allocation fails.
 *
 * @note Ownership: with @p str_provided == NULL the caller owns and must free()
 *       the returned buffer.
 *
 * @code{.c}
 * unsigned char *b64 = base64_encode((const unsigned char *)"foo", 3, NULL);
 * if (b64) { printf("%s\n", b64); free(b64); }  // "Zm9v"
 * @endcode
 */
PUBLIC_API unsigned char *base64_encode(const unsigned char *buffer, int length, unsigned char *str_provided);

/**
 * @brief Decode a base64 string into a malloc'd binary buffer.
 *
 * Decodes up to @p length characters of @p str, stopping at the first '=' pad
 * character or NUL terminator.  The decoded bytes are raw binary (not
 * NUL-terminated in general); the returned length in @p ret is the source of
 * truth.
 *
 * @param[in]  str     NUL-terminated base64 text.  Must be non-NULL.
 * @param[in]  length  Number of characters to decode.  Must be >= 0.
 * @param[out] ret     Receives the number of decoded bytes.  Must be non-NULL.
 *
 * @return malloc'd buffer of decoded bytes on success (caller frees), or NULL on
 *         failure — NULL/negative arguments, allocation failure, a character
 *         outside the base64 alphabet, or a '=' in an invalid position.
 *
 * @note Ownership: on success the caller must free() the returned buffer.
 *
 * @code{.c}
 * int n = 0;
 * unsigned char *bytes = base64_decode((const unsigned char *)"Zm9v", 4, &n);
 * if (bytes) {
 *     // bytes[0..2] == "foo"
 *     free(bytes);
 * }
 * @endcode
 */
PUBLIC_API unsigned char *base64_decode(const unsigned char *str, int length, int *ret);

/**
 * @brief Decode a base64 string leniently into a malloc'd binary buffer.
 *
 * Identical to base64_decode() but tolerant of common non-canonical encodings:
 * whitespace (spaces, tabs, CR/LF) is skipped, the URL-safe characters '-' and
 * '_' are accepted in place of '+' and '/', missing padding is allowed, and
 * over-padding/trailing garbage after the padding are ignored.
 *
 * @param[in]  str     NUL-terminated base64 text.  Must be non-NULL.
 * @param[in]  length  Number of characters to decode.  Must be >= 0.
 * @param[out] ret     Receives the number of decoded bytes.  Must be non-NULL.
 *
 * @return malloc'd buffer of decoded bytes on success (caller frees), or NULL on
 *         failure (NULL/negative arguments, allocation failure, or a character
 *         that is neither whitespace, padding, nor a base64/URL-safe digit).
 *
 * @note Ownership: on success the caller must free() the returned buffer.
 *
 * @code{.c}
 * int n = 0;
 * unsigned char *bytes = base64_decode_lenient((const unsigned char *)"T W F u", 7, &n);
 * if (bytes) {
 *     // bytes[0..2] == "Man"
 *     free(bytes);
 * }
 * @endcode
 */
PUBLIC_API unsigned char *base64_decode_lenient(const unsigned char *str, int length, int *ret);

/**
 * @brief Decode a base64 string into a caller-provided or allocated buffer.
 *
 * Identical to base64_decode() except the destination is chosen explicitly:
 * when @p decoded_in is non-NULL the decoded bytes are written into it (it must
 * be large enough); when NULL a zero-initialised buffer is calloc'd.
 *
 * @param[in]     str         NUL-terminated base64 text.  Must be non-NULL.
 * @param[in]     length      Number of characters to decode.  Must be >= 0.
 * @param[in,out] decoded_in  Optional caller-provided destination.  NULL to
 *                            allocate; otherwise must hold the decoded bytes
 *                            plus one NUL terminator.
 * @param[out]    ret         Receives the number of decoded bytes.  Must be
 *                            non-NULL.
 *
 * @return The decoded buffer — @p decoded_in when supplied, otherwise a
 *         freshly calloc'd buffer the caller must free().  Returns NULL on
 *         failure (NULL/negative arguments, allocation failure, a character
 *         outside the base64 alphabet, or a '=' in an invalid position).
 *
 * @code{.c}
 * unsigned char buf[8];
 * int n = 0;
 * base64_decode_buffered((const unsigned char *)"Zm9v", 4, buf, &n);
 * // buf[0..2] == "foo", n == 3
 * @endcode
 */
PUBLIC_API unsigned char *base64_decode_buffered(const unsigned char *str, int length, unsigned char *decoded_in, int *ret);

#endif //UFSRV_UTILS_BASE64_H
