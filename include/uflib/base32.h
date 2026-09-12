/**
 * @file base32.h
 * @brief Douglas Crockford Base32 encode/decode (libbase32) and buffer-size helpers.
 */

/**
 * Simple implementation of Douglas Crockford's  base32 encoding/decoding scheme
 *
 * Canonical information about the scheme is found at:
 * http://www.crockford.com/wrmg/base32.html
 */


#ifndef UFLIB_BASE32_H
#define UFLIB_BASE32_H

#include <uflib/uflib_defs.h>

#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

static inline void die(int code, const char *fmt, ...)
__attribute__((format(__printf__, 2, 3), noreturn));

static inline void die(int code, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  exit(code);
}

/**
 * @brief Encode a byte buffer as Crockford Base32 text.
 *
 * @details Packs each group of five input bytes into eight base-32 characters
 *          (5 bits per character) using Douglas Crockford's alphabet
 *          `0123456789ABCDEFGHJKMNPQRSTVWXYZ` — the letters I, L, O and U are
 *          deliberately excluded to reduce transcription errors.  No `=` padding
 *          is emitted, so the result is unpadded Crockford Base32 and is always
 *          NUL-terminated.
 *
 * @param[out] dest   Destination buffer.  Must hold at least
 *                    base32encsize(@p s_len) + 1 bytes.
 * @param[in]  src    Input bytes.  Must be non-NULL when @p s_len > 0.
 * @param[in]  s_len  Number of bytes to encode.
 *
 * @return Length of the encoded string in characters, excluding the NUL.
 *
 * @note For @p s_len == 0 the result is the empty string: @p dest[0] is set to
 *       NUL and 0 is returned.
 *
 * @code{.c}
 * char out[11];
 * size_t n = base32enc(out, "foobar", 6);   // out == "CSQPYRK1E8", n == 10
 * @endcode
 */
PUBLIC_API size_t base32enc(char *dest, const void *src, size_t s_len);

/**
 * @brief Decode Crockford Base32 text into a byte buffer.
 *
 * @details Decodes the NUL-terminated string @p src.  The decoded bytes are raw
 *          binary and are NOT NUL-terminated; the return value is the number of
 *          bytes written.  Decoding stops at the first invalid character.
 *
 * @param[out] dest        Destination buffer.  Must be large enough to hold the
 *                         decoded bytes.
 * @param[in]  dest_len    Size of @p dest in bytes.
 * @param[in]  src         NUL-terminated Crockford Base32 string.  Must be non-NULL.
 * @param[in]  is_lenient  When true, decoding is tolerant: lowercase letters are
 *                         accepted (case-insensitive), I and L decode as 1 and O
 *                         as 0 (Crockford's ambiguity mappings), `=` terminates
 *                         the string (padding), and `-` is skipped (readability
 *                         separators).  When false, only the canonical uppercase
 *                         alphabet `0123456789ABCDEFGHJKMNPQRSTVWXYZ` is accepted
 *                         and any other character stops decoding.
 *
 * @return Number of decoded bytes written to @p dest (capped at @p dest_len
 *         when the decoded payload is larger), or (size_t)-1 on an invalid
 *         character.
 *
 * @note When @p dest is smaller than the decoded payload, the excess bytes are
 *       truncated and the return value is capped at @p dest_len.
 *
 * @note On an invalid character (relative to @p is_lenient), decoding aborts and
 *       (size_t)-1 is returned.  A return of 0 means the input decoded to zero
 *       bytes (for example, an empty input).
 *
 * @note Reentrant and thread-safe: the implementation holds no static mutable
 *       state, so concurrent calls on independent buffers are safe.
 *
 * @code{.c}
 * unsigned char out[16];
 * size_t n = base32dec(out, sizeof(out), "CSQPYRK1E8", true);   // n == 6, out == "foobar"
 * size_t m = base32dec(out, sizeof(out), "csqpy rk1e8", true);  // tolerant: lowercase
 * @endcode
 */
PUBLIC_API size_t base32dec(void *dest, size_t dest_len, const char *src, bool is_lenient);

/* base32dec_ex() behaviour flags.  Combine with | to select decode behaviour. */
#define BASE32DEC_FLAG_LENIENT         (1u << 0)  ///< case-insensitive; I/L -> 1, O -> 0; skip '-'
#define BASE32DEC_FLAG_ACCEPT_PADDING  (1u << 1)  ///< treat '=' as a clean terminator
#define BASE32DEC_FLAG_PARTIAL_ON_ERR  (1u << 2)  ///< return bytes-so-far on invalid char (not (size_t)-1)

/**
 * @brief Decode Crockford Base32 with explicit behaviour flags.
 *
 * @details The flag-controlled superset of base32dec().  The legacy behaviour is
 *          reproduced by (BASE32DEC_FLAG_ACCEPT_PADDING | BASE32DEC_FLAG_PARTIAL_ON_ERR):
 *          strict canonical alphabet, `=` terminates, and an invalid character
 *          returns the bytes decoded so far rather than (size_t)-1.
 *
 * @param[out] dest      Destination buffer.
 * @param[in]  dest_len  Size of @p dest in bytes.
 * @param[in]  src       NUL-terminated Crockford Base32 string.
 * @param[in]  flags     Bitwise OR of BASE32DEC_FLAG_* values.
 *
 * @return Number of decoded bytes (capped at @p dest_len), or (size_t)-1 on an
 *         invalid character unless BASE32DEC_FLAG_PARTIAL_ON_ERR is set.
 *
 * @note Reentrant and thread-safe: the implementation holds no static mutable
 *       state, so concurrent calls on independent buffers are safe.
 */
PUBLIC_API size_t base32dec_ex(void *dest, size_t dest_len, const char *src, uint32_t flags);

/**
 * @brief Compute the exact unpadded encoded length for @p count input bytes.
 *
 * @param[in] count  Number of input bytes.
 *
 * @return ceil(@p count * 8 / 5) — the unpadded encoded length, excluding the
 *         NUL terminator.
 *
 * @code{.c}
 * size_t need = base32encsize(6);   // 10
 * @endcode
 */
PUBLIC_API size_t base32encsize(size_t count);

/**
 * @brief Compute the decoded byte count for @p count encoded characters.
 *
 * @param[in] count  Number of encoded characters.
 *
 * @return floor(@p count * 5 / 8) — the number of decoded bytes.
 *
 * @code{.c}
 * size_t need = base32decsize(8);   // 5
 * @endcode
 */
PUBLIC_API size_t base32decsize(size_t count);

/**
 * @brief Provide a safe (upper-bound) encoded buffer size for @p src_sz bytes.
 *
 * @details Returns the padded chunk size ceil(@p src_sz / 5) * 8, which is
 *          always at least base32encsize(@p src_sz).  Use it when sizing a
 *          destination buffer before encoding.
 *
 * @param[in] src_sz  Number of input bytes.
 *
 * @return ceil(@p src_sz / 5) * 8, excluding the NUL terminator.
 *
 * @note Computed via float division, so it is exact only for src_sz < 2^24;
 *       larger inputs may lose precision.
 *
 * @code{.c}
 * size_t cap = Base32ProvideEncodedBufferSize(6);   // 16 (>= base32encsize(6) == 10)
 * @endcode
 */
PUBLIC_API size_t Base32ProvideEncodedBufferSize(size_t src_sz);

#endif //LIBBASE32_BASE32_H
