/**
 * @file utils_hex.h
 * @brief Hexadecimal encode/decode primitives: bin2hex / bin2hex_case, hexchr2bin, hex2bin.
 *
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

#ifndef UFLIB_UTILS_HEX_H
#define UFLIB_UTILS_HEX_H

#include <uflib/uflib_defs.h>

#include <uflib/standard_c_includes.h>

#define BIN2HEX_SMALL_LETTER_ENCODING   true  ///< Lowercase hex letters (a-f).
#define BIN2HEX_DEFAULT_LETTER_ENCODING false ///< Default case for bin2hex() — uppercase (A-F).

/**
 * @brief Encode a byte buffer as an uppercase hexadecimal string.
 *
 * Each input byte is rendered as two uppercase hex digits (0-9, A-F) and a NUL
 * terminator is always appended, so the output occupies @p len*2 + 1 bytes.
 *
 * @param[in]     bin         Input bytes to encode.  Must be non-NULL.
 * @param[in]     len         Number of bytes in @p bin.  Must be > 0.
 * @param[in,out] result_out  Optional caller-provided output buffer.  When
 *                            non-NULL it must hold at least @p len*2 + 1 bytes
 *                            and receives the encoded text.  When NULL the
 *                            result is malloc'd.
 *
 * @return Pointer to the NUL-terminated hex string on success.  This equals
 *         @p result_out when one was supplied, otherwise it is a freshly
 *         malloc'd buffer the caller must free().  Returns NULL when @p bin is
 *         NULL, @p len is 0, or the malloc'd allocation fails.
 *
 * @note Convenience wrapper for bin2hex_case() with
 *       BIN2HEX_DEFAULT_LETTER_ENCODING, so the output is always uppercase;
 *       call bin2hex_case() directly for lowercase output.
 *
 * @code{.c}
 * // Malloc'd path — the caller owns and frees the result.
 * char *hex = bin2hex((const unsigned char *)"AB", 2, NULL);
 * if (hex) { printf("%s\n", hex); free(hex); }   // prints "4142"
 *
 * // Caller-supplied buffer path — no allocation.
 * char buf[5];
 * bin2hex((const unsigned char *)"AB", 2, buf);  // buf == "4142"
 * @endcode
 */
PUBLIC_API char *bin2hex(const unsigned char *bin, size_t len, char *result_out);

/**
 * @brief Encode a byte buffer as a hexadecimal string with a chosen letter case.
 *
 * Renders each input byte as two hex digits and appends a NUL terminator, so the
 * output occupies @p len*2 + 1 bytes.  The letter case is selected by
 * @p is_small_letter_hex: lowercase (0-9, a-f) when true, uppercase (0-9, A-F)
 * when false.
 *
 * @param[in]     bin                 Input bytes to encode.  Must be non-NULL.
 * @param[in]     len                 Number of bytes in @p bin.  Must be > 0.
 * @param[in]     is_small_letter_hex Letter case: true → lowercase (a-f),
 *                                    false → uppercase (A-F).
 * @param[in,out] result_out          Optional caller-provided output buffer.
 *                                    When non-NULL it must hold at least
 *                                    @p len*2 + 1 bytes and receives the text.
 *                                    When NULL the result is malloc'd.
 *
 * @return Pointer to the NUL-terminated hex string on success — equal to
 *         @p result_out when supplied, otherwise a freshly malloc'd buffer the
 *         caller must free().  Returns NULL when @p bin is NULL, @p len is 0,
 *         or the malloc'd allocation fails.
 *
 * @code{.c}
 * char *lower = bin2hex_case((const unsigned char *)"AB", 2, true, NULL);
 * if (lower) { printf("%s\n", lower); free(lower); }  // "6162"
 * @endcode
 */
PUBLIC_API char *bin2hex_case(const unsigned char *bin, size_t len, bool is_small_letter_hex, char *result_out);

/**
 * @brief Decode a single hexadecimal character into its 4-bit value.
 *
 * Accepts digits 0-9, uppercase A-F and lowercase a-f (case-insensitive).
 *
 * @param[in]  hex  The character to decode.
 * @param[out] out  Receives the decoded nibble (0-15).  Must be non-NULL.
 *
 * @return 1 on success (and @p *out is written); 0 on failure — either @p out
 *         is NULL or @p hex is not a hexadecimal digit (in which case @p *out
 *         is left untouched).
 *
 * @code{.c}
 * char nibble;
 * if (hexchr2bin('F', &nibble)) {
 *     // nibble == 15
 * }
 * @endcode
 */
PUBLIC_API int hexchr2bin(const char hex, char *out);

/**
 * @brief Decode an even-length hexadecimal string into a byte buffer.
 *
 * The input must be a NUL-terminated string of an even number of hexadecimal
 * digits (case-insensitive).  The decoded bytes are raw binary — they are NOT
 * NUL-terminated; the returned length is the source of truth.
 *
 * @param[in]  hex  NUL-terminated hex string of even length.  Must be non-NULL
 *                  and non-empty.
 * @param[out] out  Receives a malloc'd buffer of decoded bytes.  On success the
 *                  caller is responsible for free(*out).
 *
 * @return Number of decoded bytes on success.  0 on failure — @p hex is NULL or
 *         empty, @p out is NULL, the string length is odd, a non-hex digit is
 *         present, or allocation fails.
 *
 * @note Ownership: on success *out is malloc'd (strlen(hex)/2 bytes) and must
 *       be free()d by the caller.  On any failure nothing is allocated and the
 *       caller owns nothing to free; *out is NULL.
 *
 * @code{.c}
 * unsigned char *bytes = NULL;
 * size_t n = hex2bin("DEADBEEF", &bytes);
 * if (n > 0) {
 *     // bytes[0..3] == {0xDE, 0xAD, 0xBE, 0xEF}
 *     free(bytes);
 * }
 * @endcode
 */
PUBLIC_API size_t hex2bin(const char *hex, unsigned char **out);

#endif //UFLIB_UTILS_HEX_H
