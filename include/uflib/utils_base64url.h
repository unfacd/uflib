/**
 * @file utils_base64url.h
 * @brief Base64url (RFC 4648 §5) encode/decode primitives and size helper.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFLIB_UTILS_BASE64URL_H
#define UFLIB_UTILS_BASE64URL_H

#include <uflib/uflib_defs.h>

#include <stddef.h>
#include <sys/types.h>

/**
 * @brief Encode a binary buffer as a NUL-terminated, unpadded base64url string.
 *
 * Renders @p data_sz input bytes using the URL-safe alphabet (A-Z, a-z, 0-9,
 * '-', '_') with no '=' padding, and always appends a NUL terminator.  The
 * caller supplies and sizes @p result_out — at most <tt>4*ceil(data_sz/3)+1</tt>
 * bytes are needed.
 *
 * @param[in]  data       Input bytes to encode.  May be NULL only when
 *                        @p data_sz == 0.
 * @param[in]  data_sz    Number of bytes in @p data.
 * @param[out] result_out Caller-provided output buffer; receives the
 *                        NUL-terminated encoded text.  Must be non-NULL.
 *
 * @code{.c}
 * unsigned char out[8];
 * base64url_encode((const unsigned char *)"foo", 3, out); // out == "Zm9v"
 * @endcode
 */
PUBLIC_API void base64url_encode(const unsigned char *data, size_t data_sz, unsigned char *result_out);

/**
 * @brief Decode a base64url buffer into a caller-provided destination.
 *
 * Strictly decodes @p len bytes of @p data using the RFC 4648 §5 alphabet
 * (A-Z, a-z, 0-9, '-', '_'), rejecting '=' padding, the standard-alphabet
 * characters '+' and '/', whitespace, embedded NUL, and non-canonical trailing
 * bits.  On success the output is NUL-terminated.
 *
 * Unlike a NUL-terminated string API, @p data is a <tt>(pointer, length)</tt>
 * pair, so an embedded NUL is an ordinary byte that is rejected rather than
 * silently truncating the input.
 *
 * @param[in]     data       Input bytes.  Must be non-NULL.
 * @param[in]     len        Number of bytes in @p data.
 * @param[out]    result_out Caller-provided destination.  Must be non-NULL.
 * @param[in]     result_cap Capacity of @p result_out in bytes.
 *
 * @return The number of decoded bytes (>= 0) on success; -1 on invalid input or
 *         a NULL argument; -2 when @p result_cap is too small for the decoded
 *         output (size it with base64_decoded_size()).  On failure the contents
 *         of @p result_out are unspecified — do not rely on them.
 *
 * @code{.c}
 * const unsigned char in[] = "Zm9v";
 * unsigned char out[base64_decoded_size(in, sizeof in - 1)];
 * ssize_t n = base64url_decode(in, sizeof in - 1, out, sizeof out); // n == 3
 * @endcode
 */
PUBLIC_API ssize_t base64url_decode(const unsigned char *data, size_t len,
                                    unsigned char *result_out, size_t result_cap);

/**
 * @brief Compute the buffer size needed to decode a base64url buffer.
 *
 * Returns the exact decoded byte count of the valid leading base64url
 * characters of @p buf, plus one byte for the NUL terminator that
 * base64url_decode() writes — i.e. the allocation size to hand to
 * base64url_decode() as @p result_cap.
 *
 * @param[in] buf Pointer to base64url text.  May be NULL.
 * @param[in] len Number of bytes in @p buf.
 *
 * @return Required buffer size in bytes (decoded + NUL); 0 when @p buf is NULL,
 *         otherwise at least 1 (the empty input still needs its NUL).
 */
PUBLIC_API size_t base64_decoded_size(const unsigned char *buf, size_t len);

#endif /* UFLIB_UTILS_BASE64URL_H */
