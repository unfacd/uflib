/**
 * @file utils_crypto.h
 * @brief Cryptographic primitives: constant-time comparison, SHA-1/SHA-256
 *        (HMAC) digests, CSPRNG helpers, and AES-256-CBC + HMAC message
 *        encryption/decryption with a signalling key.
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

#ifndef UFLIB_UTILS_CRYPTO_H
#define UFLIB_UTILS_CRYPTO_H

#include <uflib/uflib_defs.h>

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef SHA_DIGEST_LENGTH
# define SHA_DIGEST_LENGTH 20 //bytes should be in ssl's sha.h
#endif

/*! Container for one protected message — either the encrypted (signalling)
 *  form produced by EncryptWithSignallingKey() or the decrypted plaintext
 *  form produced by DecryptWithSignallingKey().  Both directions share the
 *  same layout; the `msg` union's active member depends on the direction. */
struct CryptoMessage {
	unsigned char version[1];        ///< Protocol version byte (currently 1).
	union {
	unsigned char *msg_b64;          ///< Cipher message encoded in base64.
	unsigned char *msg_clear;        ///< Clear-text message (decrypted).
	unsigned char *msg_raw;          ///< Cipher message in binary.
	} msg;
	unsigned char *hmac;             ///< Actual (raw) HMAC digest.
	unsigned char *final_message;    ///< Entire data space the digest covers.
	unsigned char *final_message_b64;///< base64 of the final on-wire message.
	size_t size;                     ///< Payload size in bytes.
};
typedef struct CryptoMessage EncryptedMessage;
typedef struct CryptoMessage DecryptedMessage;

/**
 * @brief Compute the SHA-1 digest of a buffer as hex text or base64.
 *
 * @details Produces 20 bytes of raw digest with SHA1(), then renders it
 *          either as 40 lowercase hex characters (b64flag == 0) or as base64
 *          (b64flag != 0) into @p output.
 *
 * @param[in]  input       Input bytes.  Must be non-NULL.
 * @param[in]  input_len   Number of input bytes.
 * @param[out] output      Output buffer.  Must be at least
 *                         `SHA_DIGEST_LENGTH * 2 + 1` bytes for the hex path,
 *                         or large enough for the base64 of 20 bytes
 *                         (28 chars + NUL) for the base64 path.
 * @param[in]  output_len  Capacity of @p output in bytes.
 * @param[in]  b64flag     0 → hex, non-zero → base64.
 *
 * @return On the base64 path, the return value of b64_ntop() (the encoded
 *         length, or -1 on failure).  On the hex path, SHA_DIGEST_LENGTH * 2
 *         (40) — the length of the encoded text.
 *
 * @note The hex path does not bounds-check @p output against @p output_len.
 *
 * @code{.c}
 * char hex[SHA_DIGEST_LENGTH * 2 + 1];
 * ComputeSHA1((const unsigned char *)"abc", 3, hex, sizeof(hex), 0);
 * // hex == "a9993e364706816aba3e25717850c26c9cd0d89d"
 * @endcode
 */
PUBLIC_API int ComputeSHA1 (const unsigned char *input, size_t input_len, char *output, size_t output_len, unsigned b64flag) __attribute__((nonnull));

/**
 * @brief Compute an HMAC-SHA256 digest of @p text using @p key.
 *
 * @details A hand-rolled RFC 2104 HMAC construction over SHA-256.  Keys longer
 *          than 64 bytes are first reduced to SHA256(key).  The digest is
 *          written to @p digest, which the caller must size to
 *          SHA256_DIGEST_LENGTH (32) bytes.
 *
 * @param[in]  text      Data stream to authenticate.  Must be non-NULL.
 * @param[in]  text_len  Length of @p text in bytes.
 * @param[in]  key       Authentication key.  Must be non-NULL.
 * @param[in]  key_len   Length of @p key in bytes.
 * @param[out] digest    Caller buffer (32 bytes) receiving the digest.
 *
 * @note The inner-hash input buffer is heap-allocated (64 + @p text_len bytes),
 *       so @p text_len is not bounded by a fixed stack buffer.
 *
 * @code{.c}
 * unsigned char digest[32];
 * const unsigned char key[20] = {0};
 * ComputeHmacSha256((const unsigned char *)"data", 4, key, sizeof(key), digest);
 * @endcode
 */
PUBLIC_API void ComputeHmacSha256(const unsigned char *text, int text_len, const unsigned char *key, int key_len, void *digest) __attribute__((nonnull));

/**
 * @brief Encrypt a cleartext with a signalling key (AES-256-CBC + HMAC).
 *
 * @details Produces an EncryptedMessage whose on-wire form
 *         (@p final_message_b64) embeds a random IV, the ciphertext, and a
 *         truncated (10-byte) HMAC-SHA256.  The signalling key is 52 bytes:
 *         32-byte AES-256 cipher key followed by a 20-byte HMAC key.
 *
 * @param[in] cleartext            Plaintext bytes.  Must be non-NULL.
 * @param[in] textlen              Plaintext length in bytes.
 * @param[in] key                  Signalling key — either the 52-byte raw key
 *                                 or its base64 encoding (see @p flag).
 * @param[in] flag_b64encoded_key  true if @p key is base64-encoded, false if
 *                                 it is the raw 52 bytes.
 *
 * @return A heap-allocated EncryptedMessage on success (free with
 *         EncryptedMessageDestruct()), or NULL on failure.
 *
 * @code{.c}
 * EncryptedMessage *enc =
 *     EncryptWithSignallingKey((const unsigned char *)"hi", 2, key_b64, true);
 * if (enc) { EncryptedMessageDestruct(enc, true); }
 * @endcode
 */
PUBLIC_API EncryptedMessage *EncryptWithSignallingKey (const unsigned char *cleartext, size_t textlen, unsigned char *key, bool) __attribute__((nonnull));

/**
 * @brief Decrypt and authenticate a message produced by
 *        EncryptWithSignallingKey().
 *
 * @details Recomputes the truncated HMAC over the embedded IV + ciphertext and
 *         refuses (returns NULL) on tamper, wrong protocol version, or a
 *         malformed base64 payload.
 *
 * @param[in] cleartext        base64 ciphertext (with IV embedded).  Non-NULL.
 * @param[in] textlen          Length of the base64 string in bytes.
 * @param[in] key              Signalling key (52 raw bytes, or base64).
 * @param[in] flag_b64         true if @p key is base64-encoded.
 *
 * @return A heap-allocated DecryptedMessage on success (free with
 *         DecryptedMessageDestruct()), or NULL on any failure.
 *
 * @note On success `msg.msg_clear` holds the plaintext and `size` is the
 *       decrypted length (padded up to a full AES block — see the ciphertext
 *       layout; the true plaintext length is not separately recorded).
 *
 * @code{.c}
 * DecryptedMessage *dec = DecryptWithSignallingKey(enc->final_message_b64,
 *                     strlen((char *)enc->final_message_b64), key_b64, true);
 * if (dec) {
 *   // use dec->msg.msg_clear ...
 *   DecryptedMessageDestruct(dec, true);
 * }
 * @endcode
 */
PUBLIC_API DecryptedMessage *DecryptWithSignallingKey (const unsigned char *cleartext, size_t textlen, unsigned char *key, bool flag_b64) __attribute__((nonnull));

/**
 * @brief Free an EncryptedMessage, optionally freeing the struct itself.
 *
 * @param[in] enc_ptr            Message to free.  Must be non-NULL.
 * @param[in] flag_selfdestruct  true → also free @p enc_ptr; false → leave it
 *                               (zeroed) for the caller to free.
 *
 * @code{.c}
 * EncryptedMessageDestruct(enc, true); // frees all owned buffers + the struct
 * @endcode
 */
PUBLIC_API void EncryptedMessageDestruct (EncryptedMessage *enc_ptr, bool flag_selfdestruct) __attribute__((nonnull));

/**
 * @brief Free a DecryptedMessage, optionally freeing the struct itself.
 *
 * @param[in] denc_ptr           Message to free.  Must be non-NULL.
 * @param[in] flag_selfdestruct  true → also free @p denc_ptr.
 *
 * @code{.c}
 * DecryptedMessageDestruct(dec, true);
 * @endcode
 */
PUBLIC_API void DecryptedMessageDestruct (DecryptedMessage *denc_ptr, bool flag_selfdestruct) __attribute__((nonnull));

/**
 * @brief Generate a 64-bit pseudo-random number (rand() based, not CSPRNG).
 *
 * @return A value in [0, ULONG_MAX].
 *
 * @note Seeded via SeedRandom() or the process default rand() seed.  For
 *       cryptographically strong output prefer GenerateSecureRandom().
 *
 * @code{.c}
 * unsigned long r = GenerateRandomNumber();   // pseudo-random 64-bit value
 * @endcode
 */
PUBLIC_API unsigned long GenerateRandomNumber ();

/**
 * @brief Generate a uniformly distributed value in [0, max].
 *
 * @param[in] max  Upper bound (inclusive).  Must satisfy 0 <= max <= RAND_MAX.
 *
 * @return A value in [0, max].
 *
 * @note Out-of-range @p max (negative or > RAND_MAX) returns 0.
 *
 * @code{.c}
 * unsigned long r = GenerateRandomNumberWithUpper(10);   // value in [0, 10]
 * @endcode
 */
PUBLIC_API unsigned long GenerateRandomNumberWithUpper (long max);

/**
 * @brief Compute a bounded exponential-backoff value from a recurrence count.
 *
 * @param[in] recurrence  Successive increment (state kept by the caller).
 * @param[in] min         Lower bound of the returned value.
 * @param[in] max         Upper bound of the returned value.
 *
 * @return `min` plus a quadratic-in-@p recurrence term, clamped to @p max.
 *
 * @code{.c}
 * int backoff = GetNextExponentialBackoffValue(3, 100, 5000);
 * @endcode
 */
PUBLIC_API int GetNextExponentialBackoffValue(int recurrence, int min, int max);

/**
 * @brief Generate a random number in [min, max).
 *
 * @param[in] min  Lower bound (inclusive).
 * @param[in] max  Upper bound (exclusive).
 *
 * @return A value in [min, max).
 *
 * @note Not thread-safe (uses rand()).  When min >= max, @p min is returned.
 *
 * @code{.c}
 * long r = GenerateRandomNumberBounded(10, 20);   // value in [10, 20)
 * @endcode
 */
PUBLIC_API long GenerateRandomNumberBounded (long min, long max);

/**
 * @brief Fill @p data with @p len cryptographically secure random bytes.
 *
 * @param[out] data  Buffer to fill.  Must be non-NULL.
 * @param[in]  len   Number of bytes to fill.
 *
 * @return 0 (always).
 *
 * @code{.c}
 * uint8_t key[32];
 * GenerateSecureRandom(key, sizeof(key));
 * @endcode
 */
PUBLIC_API int GenerateSecureRandom (uint8_t *data, size_t len) __attribute__((nonnull));

/**
 * @brief Generate a cryptographically secure random hex string.
 *
 * @param[in] buffer_ptr_provided  Optional pre-allocated buffer of
 *                                 buffer_sz + 1 bytes, or NULL to
 *                                 heap-allocate.
 * @param[in] buffer_sz            Number of hex characters to produce.  The
 *                                 result is exactly @p buffer_sz chars,
 *                                 NUL-terminated; odd values round the
 *                                 random-byte count up.
 *
 * @return The hex string — @p buffer_ptr_provided if supplied, otherwise a
 *         heap-allocated buffer the caller must free().  The result is always
 *         NUL-terminated.
 *
 * @code{.c}
 * char *hex = GenerateSecureRandomHexed(NULL, 32);   // 32 hex chars, NUL-terminated
 * if (hex) {
 *   // use hex ...
 *   free(hex);
 * }
 * @endcode
 */
PUBLIC_API char *GenerateSecureRandomHexed(char *buffer_ptr_provided, size_t buffer_sz);

/**
 * @brief Generate a random salt and return it as lowercase hex text.
 *
 * @param[in] length           Number of random bytes (128 for strong output).
 * @param[in] zero_terminated  Whether to NUL-terminate the returned string.
 *
 * @return A heap-allocated hex string (free() by the caller), or NULL if the
 *         underlying RAND_bytes() call fails.
 *
 * @code{.c}
 * unsigned char *salt = GenerateSalt(128, true);   // 128 bytes -> 256 lowercase hex chars
 * if (salt) {
 *   // use salt ...
 *   free(salt);
 * }
 * @endcode
 */
PUBLIC_API unsigned char *GenerateSalt (unsigned length, bool zero_terminated);

/**
 * @brief Render @p len bytes as uppercase hex text.
 *
 * @param[in] pv         Bytes to render.  Must be non-NULL.
 * @param[in] len        Number of bytes.
 * @param[in] outbuffer  Optional caller buffer (len*2 + 1 bytes), else heap.
 *
 * @return The hex text — @p outbuffer if supplied, otherwise heap-allocated.
 *
 * @code{.c}
 * const unsigned char raw[2] = {0xDE, 0xAD};
 * unsigned char *hex = hex_print(raw, 2, NULL);   // "DEAD"
 * if (hex) {
 *   // use hex ...
 *   free(hex);
 * }
 * @endcode
 */
PUBLIC_API unsigned char *hex_print(const unsigned char *pv, size_t len, unsigned char *outbuffer) __attribute__((nonnull(1)));

/**
 * @brief Compare two buffers in constant time.
 *
 * @param[in] a     First buffer.  Must be non-NULL (unless size == 0).
 * @param[in] b     Second buffer.  Must be non-NULL (unless size == 0).
 * @param[in] size  Number of bytes to compare.
 *
 * @return 0 if equal, non-zero otherwise.
 *
 * @code{.c}
 * if (strcmp_constant_time(a, b, n) == 0) {
 *   // the first n bytes of a and b are equal
 * }
 * @endcode
 */
PUBLIC_API int strcmp_constant_time(const void *a, const void *b, const size_t size) __attribute__((nonnull));

/**
 * @brief Compare two NUL-terminated strings in constant time.
 *
 * @param[in] a  First string.  Must be non-NULL.
 * @param[in] b  Second string.  Must be non-NULL.
 *
 * @return 0 if equal, non-zero otherwise.
 *
 * @note Strings longer than MBUF bytes are rejected (non-zero returned)
 *       without comparison, to avoid leaking length information.
 *
 * @code{.c}
 * if (strcmp_time_constant2(a, b) == 0) {
 *   // strings are equal
 * }
 * @endcode
 */
PUBLIC_API int strcmp_time_constant2 (char *a, char *b) __attribute__((nonnull));

/**
 * @brief Compare two buffers in constant time.
 *
 * @param[in] s1  First buffer.
 * @param[in] s2  Second buffer.
 * @param[in] n   Number of bytes to compare.
 *
 * @return 0 if equal, non-zero otherwise.
 *
 * @code{.c}
 * if (memcpy_constant_time(a, b, n) == 0) {
 *   // the first n bytes of a and b are equal
 * }
 * @endcode
 */
PUBLIC_API int memcpy_constant_time(const void *s1, const void *s2, size_t n);

/**
 * @brief Compare two buffers in constant time.
 *
 * @param[in] s1  First buffer.
 * @param[in] s2  Second buffer.
 * @param[in] n   Number of bytes to compare.
 *
 * @return 0 if equal, non-zero otherwise.
 *
 * @code{.c}
 * if (memcmp_constant_time(a, b, n) == 0) {
 *   // the first n bytes of a and b are equal
 * }
 * @endcode
 */
PUBLIC_API int memcmp_constant_time (const void *s1, const void *s2, size_t n);

/**
 * @brief Copy @p n bytes from @p src to @p dest in constant time.
 *
 * @details A byte-for-byte copy whose control flow and memory access pattern
 *          do not depend on the byte values, so its timing depends only on
 *          @p n.  Behaviourally equivalent to memcpy(); the explicit loop
 *          keeps the copy free of data-dependent branches on secret material.
 *
 * @param[out] dest  Destination buffer.  Must be non-NULL and large enough
 *                   for @p n bytes.
 * @param[in]  src   Source buffer.  Must be non-NULL.
 * @param[in]  n     Number of bytes to copy.
 *
 * @return @p dest, as with memcpy(), for call chaining.
 *
 * @note As with memcpy(), @p dest and @p src must not overlap.
 *
 * @code{.c}
 * unsigned char secret[32], copy[32];
 * memcpy_constant_time2(copy, secret, sizeof(secret));
 * @endcode
 */
PUBLIC_API void *memcpy_constant_time2(void *dest, const void *src, size_t n);

#endif
