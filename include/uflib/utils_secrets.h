/**
 * @file utils_secrets.h
 * @brief AES-256-GCM envelopes for config-file secrets: the primitives that
 *        produce and consume `enc:v1:<nonce>:<ciphertext>:<tag>` values.
 *
 * A configuration file is readable by operators, is copied between hosts and is
 * frequently kept in version control, so a value that must stay secret cannot be
 * written into it in the clear.  It is written as a self-describing envelope
 * instead: the version tag names the scheme, and the three hex components are
 * what the scheme needs.  The key lives elsewhere and is never in the document.
 *
 * ## The envelope
 *
 * @code
 *   enc:v1:<nonce_hex>:<ciphertext_hex>:<tag_hex>
 * @endcode
 *
 * @c v1 is AES-256-GCM with a 12-byte nonce and a 16-byte authentication tag.
 * The nonce is drawn fresh per encryption, so encrypting the same plaintext
 * twice yields different envelopes and an observer learns nothing from
 * repetition.  GCM is authenticated, so a wrong key and a tampered ciphertext
 * are the same failure — the tag does not verify — and neither is distinguishable
 * from the other by design.
 *
 * ## Failing closed
 *
 * @ref UflibSecretDecrypt refuses a value that carries no @c enc:v1: prefix
 * rather than returning it unchanged.  This is a deliberate difference from the
 * older @c SecretConfigDecrypt in ufsrvcorelib, which passes such a value
 * through, and it matters wherever a caller has already declared that a value is
 * encrypted: were the prefix optional, deleting four characters from a config
 * file would convert a secret into an accepted plaintext, and the declaration
 * would enforce nothing.  A caller that wants the pass-through behaviour can
 * test @ref UflibSecretHasPrefix itself and decide.
 *
 * ## Key material
 *
 * A key is 32 raw bytes and is carried as 64 hex characters wherever it is
 * stored as text.  @ref UflibSecretWipe exists because freeing key material or
 * plaintext does not remove it from memory: the allocator keeps the contents,
 * and a core dump captures them.
 *
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

#ifndef UFLIB_UTILS_SECRETS_H
#define UFLIB_UTILS_SECRETS_H

#include <uflib/uflib_defs.h>

#include <stddef.h>

/*!
 * The key size, in bytes.  AES-256 takes a 32-byte key.
 */
#define UFLIB_SECRET_KEY_BYTES   32

/*!
 * The nonce size, in bytes.
 *
 * 12 is the size GCM is specified for and the size it is fastest at: any other
 * length is hashed down internally, so a longer nonce is not stronger.
 */
#define UFLIB_SECRET_NONCE_BYTES 12

/*!
 * The authentication tag size, in bytes.
 */
#define UFLIB_SECRET_TAG_BYTES   16

/*!
 * The scheme marker every envelope begins with.
 */
#define UFLIB_SECRET_PREFIX      "enc:v1:"

/*!
 * Length of @ref UFLIB_SECRET_PREFIX, excluding the terminating NUL.
 */
#define UFLIB_SECRET_PREFIX_LEN  (sizeof(UFLIB_SECRET_PREFIX) - 1)

/*!
 * How a secret operation ended.
 *
 * The failures are separated rather than collapsed into a single "failed"
 * because a caller has to act on them differently: a missing prefix is a
 * configuration that was never encrypted, a missing key is a provisioning
 * mistake, and an authentication failure is a wrong key or a tampered file.
 */
typedef enum UflibSecretStatus
{
  UFLIB_SECRET_OK = 0,             ///< completed
  UFLIB_SECRET_ERR_INVALID_ARG,    ///< a NULL or empty argument
  UFLIB_SECRET_ERR_NO_PREFIX,      ///< the value is not an envelope
  UFLIB_SECRET_ERR_MALFORMED,      ///< the envelope's structure is wrong
  UFLIB_SECRET_ERR_KEY_FORMAT,     ///< key material is not the expected size or shape
  UFLIB_SECRET_ERR_AUTH,           ///< the tag did not verify: wrong key, or tampered
  UFLIB_SECRET_ERR_NO_MEMORY,      ///< allocation failed
  UFLIB_SECRET_ERR_IO,             ///< a file could not be read
  UFLIB_SECRET_ERR_NOT_FOUND       ///< the file holds no entry for that path
} UflibSecretStatus;

/*!
 * A secrets file, read and parsed.
 *
 * Opaque because the layout is not part of the contract; the file's *format* is,
 * and it is documented on @ref UflibSecretFileOpen.
 */
typedef struct UflibSecretFile UflibSecretFile;

/*!
 * @brief A stable name for a status, for diagnostics.
 *
 * @param[in] s  The status.
 *
 * @return A static string.  Never NULL, and never to be freed.
 *
 * @code{.c}
 * if (st != UFLIB_SECRET_OK)
 *     fprintf(stderr, "secret: %s\n", UflibSecretStatusString(st));
 * @endcode
 */
PUBLIC_API const char *UflibSecretStatusString(UflibSecretStatus s);

/*!
 * @brief Whether @p value carries the envelope marker.
 *
 * Exposed so a caller can decide for itself whether an unprefixed value is
 * acceptable, which @ref UflibSecretDecrypt deliberately will not do.
 *
 * @param[in] value  The value, or NULL.
 *
 * @return 1 when @p value begins with @ref UFLIB_SECRET_PREFIX, else 0.
 *
 * @code{.c}
 * if (!UflibSecretHasPrefix(v)) // the document states this in the clear
 * @endcode
 */
PUBLIC_API int UflibSecretHasPrefix(const char *value);

/*!
 * @brief Overwrites @p n bytes at @p p so they cannot be recovered.
 *
 * A plain @c memset is removed by the optimiser when the buffer is not read
 * afterwards, which is exactly the situation this exists for, so this reaches
 * for @c OPENSSL_cleanse.  Call it before freeing key material or a decrypted
 * buffer, never after.
 *
 * @param[in] p  The buffer.  NULL is accepted and does nothing.
 * @param[in] n  Bytes to overwrite.
 *
 * @code{.c}
 * UflibSecretWipe(plaintext, plaintext_len);
 * free(plaintext);
 * @endcode
 */
PUBLIC_API void UflibSecretWipe(void *p, size_t n);

/*!
 * @brief Decrypts an envelope into a freshly allocated plaintext.
 *
 * @param[in]  envelope           The value, which must begin with
 *                                @ref UFLIB_SECRET_PREFIX.
 * @param[in]  key                The 32-byte key.  Never modified.
 * @param[out] plaintext_out      Receives a malloc'd buffer, always
 *                                NUL-terminated.  The caller owns it and should
 *                                @ref UflibSecretWipe it before freeing.
 * @param[out] plaintext_len_out  Receives the plaintext's length in bytes, which
 *                                may be 0.  May be NULL.  This is reported
 *                                separately from the terminator because a
 *                                secret may legitimately contain a NUL byte.
 *
 * @return @ref UFLIB_SECRET_OK on success.
 * @return @ref UFLIB_SECRET_ERR_NO_PREFIX if @p envelope is not an envelope —
 *         deliberately *not* a pass-through.
 * @return @ref UFLIB_SECRET_ERR_MALFORMED if the three components are missing,
 *         empty where they may not be, not hex, or the wrong length.
 * @return @ref UFLIB_SECRET_ERR_AUTH if the tag did not verify.
 *
 * @note An empty plaintext is legal and is *not* the same as an empty
 *       ciphertext field being malformed: GCM over zero bytes yields a
 *       zero-length ciphertext, so `enc:v1:<nonce>::<tag>` decrypts to "".
 *
 * @code{.c}
 * char *plaintext = NULL;
 * size_t len = 0;
 * UflibSecretStatus st = UflibSecretDecrypt(value, key, &plaintext, &len);
 * if (st != UFLIB_SECRET_OK) return st;
 * // ... use plaintext[0..len-1] ...
 * UflibSecretWipe(plaintext, len);
 * free(plaintext);
 * @endcode
 */
PUBLIC_API UflibSecretStatus UflibSecretDecrypt(const char *envelope,
                                                const unsigned char key[UFLIB_SECRET_KEY_BYTES],
                                                char **plaintext_out, size_t *plaintext_len_out);

/*!
 * @brief Encrypts @p plaintext into a freshly allocated envelope.
 *
 * The inverse of @ref UflibSecretDecrypt, and what makes an envelope in the
 * first place: the nonce is drawn from the CSPRNG on every call, so no two
 * calls over the same input produce the same output.
 *
 * @param[in]  plaintext      The secret.  May be NULL only when
 *                            @p plaintext_len is 0.
 * @param[in]  plaintext_len  Its length.  Zero is legal.
 * @param[in]  key            The 32-byte key.  Never modified.
 * @param[out] envelope_out   Receives a malloc'd, NUL-terminated envelope in the
 *                            lowercase hex the scheme is written in.
 *
 * @return @ref UFLIB_SECRET_OK on success, or
 *         @ref UFLIB_SECRET_ERR_NO_MEMORY / @ref UFLIB_SECRET_ERR_INVALID_ARG.
 *
 * @code{.c}
 * char *envelope = NULL;
 * if (UflibSecretEncrypt(secret, strlen(secret), key, &envelope) == UFLIB_SECRET_OK) {
 *     printf("password = \"%s\"\n", envelope);   // paste into the config file
 *     free(envelope);
 * }
 * @endcode
 */
PUBLIC_API UflibSecretStatus UflibSecretEncrypt(const char *plaintext, size_t plaintext_len,
                                                const unsigned char key[UFLIB_SECRET_KEY_BYTES],
                                                char **envelope_out);

/*!
 * @brief Draws a new key from the system CSPRNG.
 *
 * @param[out] key  Receives @ref UFLIB_SECRET_KEY_BYTES bytes.
 *
 * @return @ref UFLIB_SECRET_OK, or @ref UFLIB_SECRET_ERR_INVALID_ARG, or
 *         @ref UFLIB_SECRET_ERR_IO if the CSPRNG could not be read.
 *
 * @code{.c}
 * unsigned char key[UFLIB_SECRET_KEY_BYTES];
 * if (UflibSecretGenerateKey(key) == UFLIB_SECRET_OK) {
 *     // ... write it as 64 hex chars to the secrets file ...
 *     UflibSecretWipe(key, sizeof(key));
 * }
 * @endcode
 */
PUBLIC_API UflibSecretStatus UflibSecretGenerateKey(unsigned char key[UFLIB_SECRET_KEY_BYTES]);

/*!
 * @brief Decodes a key from exactly 64 hex characters.
 *
 * Both letter cases are accepted; @ref UflibSecretEncrypt always emits lowercase.
 *
 * @param[in]  hex  The key as text.
 * @param[out] key  Receives @ref UFLIB_SECRET_KEY_BYTES bytes.
 *
 * @return @ref UFLIB_SECRET_OK, or @ref UFLIB_SECRET_ERR_KEY_FORMAT when the
 *         length or the digits are wrong.
 *
 * @code{.c}
 * unsigned char key[UFLIB_SECRET_KEY_BYTES];
 * if (UflibSecretKeyFromHex(line, key) != UFLIB_SECRET_OK) return -1;
 * @endcode
 */
PUBLIC_API UflibSecretStatus UflibSecretKeyFromHex(const char *hex,
                                                   unsigned char key[UFLIB_SECRET_KEY_BYTES]);

/*!
 * @brief Reads a key from a file holding exactly 32 raw bytes.
 *
 * @param[in]  path  The file.
 * @param[out] key   Receives @ref UFLIB_SECRET_KEY_BYTES bytes.
 *
 * @return @ref UFLIB_SECRET_OK, or @ref UFLIB_SECRET_ERR_KEY_FORMAT if the file
 *         is not exactly 32 bytes, or @ref UFLIB_SECRET_ERR_IO.
 *
 * @code{.c}
 * unsigned char key[UFLIB_SECRET_KEY_BYTES];
 * UflibSecretStatus st = UflibSecretKeyFromRawFile("/run/key.bin", key);
 * @endcode
 */
PUBLIC_API UflibSecretStatus UflibSecretKeyFromRawFile(const char *path,
                                                       unsigned char key[UFLIB_SECRET_KEY_BYTES]);

/*!
 * @brief Decodes a key from standard base64.
 *
 * @param[in]  b64  The encoded key.
 * @param[out] key  Receives @ref UFLIB_SECRET_KEY_BYTES bytes.
 *
 * @return @ref UFLIB_SECRET_OK, or @ref UFLIB_SECRET_ERR_KEY_FORMAT when it does
 *         not decode to exactly 32 bytes.
 *
 * @code{.c}
 * unsigned char key[UFLIB_SECRET_KEY_BYTES];
 * UflibSecretStatus st = UflibSecretKeyFromBase64(argv[1], key);
 * @endcode
 */
PUBLIC_API UflibSecretStatus UflibSecretKeyFromBase64(const char *b64,
                                                      unsigned char key[UFLIB_SECRET_KEY_BYTES]);

/*!
 * @brief Derives a key from a passphrase as its SHA-256 digest.
 *
 * A convenience for a deployment that has a passphrase rather than a key file.
 * SHA-256 is a fast hash and is *not* a password-hardening function, so this
 * gives a key no stronger than the passphrase's own entropy; it exists to make a
 * short input usable, not to make a weak one safe.
 *
 * @param[in]  text  The passphrase, as UTF-8.
 * @param[out] key   Receives @ref UFLIB_SECRET_KEY_BYTES bytes.
 *
 * @return @ref UFLIB_SECRET_OK, or @ref UFLIB_SECRET_ERR_INVALID_ARG.
 *
 * @code{.c}
 * unsigned char key[UFLIB_SECRET_KEY_BYTES];
 * UflibSecretStatus st = UflibSecretKeyFromText("correct horse battery staple", key);
 * @endcode
 */
PUBLIC_API UflibSecretStatus UflibSecretKeyFromText(const char *text,
                                                    unsigned char key[UFLIB_SECRET_KEY_BYTES]);

/*!
 * @brief Reads and parses a secrets file.
 *
 * The file maps a field's full node path to its key, one entry per line:
 *
 * @code
 *   # comments run to the end of the line
 *   ufsrv.ufnet.db_backend.password=3f2a...e9
 *   ufsrv.ssl_command_console.key_file=9c11...47
 * @endcode
 *
 * So a field declared @c encrypted in the schema is decrypted with the key
 * written against its own path, and different fields can hold different keys.  A
 * line whose key is not exactly 64 hex characters, or which carries no @c =, is
 * a malformed file and is refused rather than skipped: a silently ignored line
 * becomes a missing key later, and a missing key is not something to discover
 * one field at a time.
 *
 * @param[in]  path  The file.
 * @param[out] out   Receives the parsed file.
 *
 * @return @ref UFLIB_SECRET_OK on success.
 * @return @ref UFLIB_SECRET_ERR_IO if it cannot be read.
 * @return @ref UFLIB_SECRET_ERR_MALFORMED if a line is not `path=key`, or a key
 *         is not 64 hex characters.
 *
 * @code{.c}
 * UflibSecretFile *f = NULL;
 * if (UflibSecretFileOpen("./ufconfig.secrets", &f) != UFLIB_SECRET_OK) return -1;
 * unsigned char key[UFLIB_SECRET_KEY_BYTES];
 * UflibSecretStatus st = UflibSecretFileLookup(f, "ufsrv.ufnet.db_backend.password", key);
 * UflibSecretFileClose(f);
 * @endcode
 */
PUBLIC_API UflibSecretStatus UflibSecretFileOpen(const char *path, UflibSecretFile **out);

/*!
 * @brief The path a secrets file was opened from.
 *
 * @param[in] f  The file.
 *
 * @return A borrowed string, or NULL.  Valid until @ref UflibSecretFileClose.
 *
 * @code{.c}
 * fprintf(stderr, "secrets read from %s\n", UflibSecretFilePath(f));
 * @endcode
 */
PUBLIC_API const char *UflibSecretFilePath(const UflibSecretFile *f);

/*!
 * @brief Whether the file holds an entry for @p path.
 *
 * Separate from @ref UflibSecretFileLookup so that a caller resolving a key for
 * every declared field can report *which* paths are unaccounted for, rather than
 * only that one of them is.
 *
 * @param[in]  f     The file.
 * @param[in]  path  The full node path.
 *
 * @return 1 when an entry exists, else 0.
 *
 * @code{.c}
 * if (!UflibSecretFileHas(f, d->path)) // the schema declares a secret nobody keyed
 * @endcode
 */
PUBLIC_API int UflibSecretFileHas(const UflibSecretFile *f, const char *path);

/*!
 * @brief Resolves the key for @p path.
 *
 * @param[in]  f     The file.
 * @param[in]  path  The full node path.
 * @param[out] key   Receives @ref UFLIB_SECRET_KEY_BYTES bytes.
 *
 * @return @ref UFLIB_SECRET_OK, or @ref UFLIB_SECRET_ERR_NOT_FOUND when the file
 *         holds no entry for @p path, or @ref UFLIB_SECRET_ERR_INVALID_ARG.
 *
 * @code{.c}
 * unsigned char key[UFLIB_SECRET_KEY_BYTES];
 * if (UflibSecretFileLookup(f, d->path, key) != UFLIB_SECRET_OK) return -1;
 * @endcode
 */
PUBLIC_API UflibSecretStatus UflibSecretFileLookup(const UflibSecretFile *f, const char *path,
                                                   unsigned char key[UFLIB_SECRET_KEY_BYTES]);

/*!
 * @brief Wipes and releases a secrets file.
 *
 * Every key it holds is wiped first.  NULL is accepted and does nothing.
 *
 * @param[in] f  The file.
 *
 * @code{.c}
 * UflibSecretFileClose(f);
 * @endcode
 */
PUBLIC_API void UflibSecretFileClose(UflibSecretFile *f);

#endif /* UFLIB_UTILS_SECRETS_H */
