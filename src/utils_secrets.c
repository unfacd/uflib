/**
 * @file utils_secrets.c
 * @brief AES-256-GCM envelopes for config-file secrets.
 *
 * The scheme is a single self-describing string —
 * `enc:v1:<nonce_hex>:<ciphertext_hex>:<tag_hex>` — so a value carries
 * everything needed to interpret it except the key.  The version tag is there
 * because the scheme will change and a value written under v1 has to remain
 * readable when it does.
 *
 * Both halves of the scheme live here.  The decrypt half was ported from
 * `ufsrvcorelib`'s `ufsrv_config_secret.c`; the encrypt half was back-ported
 * from `encrypt_plaintext.py` in the same repository, which was the only
 * implementation that could produce an envelope.  uflib sits below
 * ufsrvcorelib and may not depend on it, so neither could be reused directly.
 * The two are byte-compatible: an envelope produced by the Python script
 * decrypts here and vice versa, which is asserted by a test holding a fixture
 * the script generated.
 *
 * Two deliberate departures from the ported sources:
 *
 *  - @ref UflibSecretDecrypt refuses a value with no `enc:v1:` prefix where the
 *    original returned it unchanged.  See the header for why.
 *
 *  - An empty ciphertext field is accepted.  GCM over zero bytes yields a
 *    zero-length ciphertext, so the Python script quite correctly emits
 *    `enc:v1:<nonce>::<tag>` for an empty secret — and the original C decoder
 *    rejected an empty hex string as malformed, so that value could be produced
 *    but never read.  Empty plaintext is a legal secret; an empty *field* is
 *    only illegal for the nonce and the tag, which have fixed lengths.
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

#include <uflib/standard_c_includes.h>
#include <uflib/standard_defs.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <uflib/utils_secrets.h>
#include <uflib/utils_base64.h>
#include <uflib/utils_hex.h>

/*!
 * @brief Fills @p buf with @p len bytes from the CSPRNG.
 *
 * Deliberately OpenSSL's RAND_bytes rather than the library's own
 * @c GenerateSecureRandom, which wraps @c arc4random_buf and would be the more
 * obvious reuse.  That wrapper lives in @c utils_crypto.c, and referencing it
 * makes every target that links libuflib pull that object in, then
 * @c utils_time.o for its @c set_time, then @c utils_str.o for *its*
 * @c mstrlcpy, and @c utils_str.o is the one object in the library that needs
 * utf8proc — which uflib does not name in its exported interface.  A single call
 * here would therefore have made every consumer of the config module add a
 * link dependency on a library it never previously needed.
 *
 * RAND_bytes is the same OpenSSL CSPRNG that @c utils_crypto.c itself reaches
 * for, so this is not a weaker source; it keeps this file's dependencies inside
 * OpenSSL, where its AES and SHA usage already are.
 *
 * @return 0 on success, non-zero if the CSPRNG could not be read.
 */
static int sRandomBytes(unsigned char *buf, size_t len)
{
  if (len > (size_t)INT_MAX) return -1;
  return RAND_bytes(buf, (int)len) == 1 ? 0 : -1;
}

/*!
 * Longest line the secrets file may carry, including the terminator.
 *
 * A secrets file is a handful of `path=key` lines, so this is far above any real
 * entry.  It is a ceiling rather than a dynamic read because a line that exceeds
 * it is a file that is not a secrets file, and refusing is the right answer.
 */
#define PRIV_UFLIB_SECRET_LINE_MAX 1024

/*!
 * One `path=key` entry of a secrets file.
 */
typedef struct UflibSecretSlot
{
  char *        path; ///< full node path, owned
  unsigned char key[UFLIB_SECRET_KEY_BYTES]; ///< the key, owned
} UflibSecretSlot;

struct UflibSecretFile
{
  char *           path;  ///< the file it was read from, owned
  UflibSecretSlot *slots; ///< entries, in the order the file gave them
  size_t           n;     ///< entries used
  size_t           cap;   ///< entries allocated
};

PUBLIC_API const char *UflibSecretStatusString(UflibSecretStatus s)
{
  switch (s) {
  case UFLIB_SECRET_OK:                return "OK";
  case UFLIB_SECRET_ERR_INVALID_ARG:   return "INVALID_ARG";
  case UFLIB_SECRET_ERR_NO_PREFIX:     return "NO_PREFIX";
  case UFLIB_SECRET_ERR_MALFORMED:     return "MALFORMED";
  case UFLIB_SECRET_ERR_KEY_FORMAT:    return "KEY_FORMAT";
  case UFLIB_SECRET_ERR_AUTH:          return "AUTH";
  case UFLIB_SECRET_ERR_NO_MEMORY:     return "NO_MEMORY";
  case UFLIB_SECRET_ERR_IO:            return "IO";
  case UFLIB_SECRET_ERR_NOT_FOUND:     return "NOT_FOUND";
  default:                             return "?";
  }
}

PUBLIC_API int UflibSecretHasPrefix(const char *value)
{
  if (!value) return 0;
  return strncmp(value, UFLIB_SECRET_PREFIX, UFLIB_SECRET_PREFIX_LEN) == 0;
}

PUBLIC_API void UflibSecretWipe(void *p, size_t n)
{
  if (!p || !n) return;
  /* OPENSSL_cleanse is called through a pointer the compiler cannot see
     through, which is what stops it being elided the way a memset over a buffer
     that is never read again would be. */
  OPENSSL_cleanse(p, n);
}

/*!
 * @brief Decodes @p hex into exactly @p out_len bytes.
 *
 * Length is checked before anything is decoded, so a truncated nonce or tag is
 * a malformed envelope rather than a short read that the cipher then interprets.
 * The temporary `hex2bin` allocates is wiped, because for a key it holds key
 * material.
 */
static UflibSecretStatus sHexDecodeFixed(const char *hex, unsigned char *out, size_t out_len)
{
  if (!hex || !out) return UFLIB_SECRET_ERR_INVALID_ARG;
  if (strlen(hex) != out_len * 2) return UFLIB_SECRET_ERR_MALFORMED;

  unsigned char *tmp = NULL;
  size_t         got = hex2bin(hex, &tmp);
  if (got != out_len || !tmp) {
    if (tmp) {
      UflibSecretWipe(tmp, got);
      free(tmp);
    }
    return UFLIB_SECRET_ERR_MALFORMED;
  }

  memcpy(out, tmp, out_len);
  UflibSecretWipe(tmp, got);
  free(tmp);
  return UFLIB_SECRET_OK;
}

/*!
 * @brief Decodes a variable-length hex field, which may legitimately be empty.
 *
 * An empty field yields a zero-length buffer and UFLIB_SECRET_OK, because a
 * zero-length ciphertext is what GCM produces for a zero-length plaintext.  Any
 * other length must be even and entirely hex; `hex2bin` returns 0 both for a
 * malformed string and for an empty one, which is exactly why the empty case is
 * decided here rather than inferred from its return value.
 */
static UflibSecretStatus sHexDecodeVarying(const char *hex, unsigned char **out, size_t *out_len)
{
  if (!hex || !out || !out_len) return UFLIB_SECRET_ERR_INVALID_ARG;

  size_t len = strlen(hex);
  if (len == 0) {
    unsigned char *empty = (unsigned char *)calloc(1, 1);
    if (!empty) return UFLIB_SECRET_ERR_NO_MEMORY;
    *out     = empty;
    *out_len = 0;
    return UFLIB_SECRET_OK;
  }
  if ((len % 2) != 0) return UFLIB_SECRET_ERR_MALFORMED;

  unsigned char *tmp = NULL;
  size_t         got = hex2bin(hex, &tmp);
  if (!tmp || got != len / 2) {
    if (tmp) {
      UflibSecretWipe(tmp, got);
      free(tmp);
    }
    return UFLIB_SECRET_ERR_MALFORMED;
  }
  *out     = tmp;
  *out_len = got;
  return UFLIB_SECRET_OK;
}

/*!
 * @brief AES-256-GCM authentication and decryption.
 *
 * Allocates a NUL-terminated plaintext into @p plaintext_out, which the caller
 * must wipe and free.
 */
static UflibSecretStatus sAes256GcmDecrypt(const unsigned char *key,
                                           const unsigned char *nonce, size_t nonce_len,
                                           const unsigned char *ct, size_t ct_len,
                                           const unsigned char *tag, size_t tag_len,
                                           char **plaintext_out, size_t *plaintext_len_out)
{
  EVP_CIPHER_CTX *ctx      = NULL;
  unsigned char * buf      = NULL;
  char *          result   = NULL;
  int             out_len  = 0;
  int             final_len = 0;
  UflibSecretStatus st     = UFLIB_SECRET_ERR_AUTH;

  ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return UFLIB_SECRET_ERR_NO_MEMORY;

  /* ct_len + 1 so the plaintext can be terminated; the tag is not ciphertext
     and does not contribute a byte to the output. */
  buf = (unsigned char *)malloc(ct_len + 1);
  if (!buf) {
    st = UFLIB_SECRET_ERR_NO_MEMORY;
    goto cleanup;
  }

  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)nonce_len, NULL) != 1) goto cleanup;
  if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto cleanup;
  if (EVP_DecryptUpdate(ctx, buf, &out_len, ct, (int)ct_len) != 1) goto cleanup;

  /* The tag is set before the final call, which is where GCM checks it.  This is
     the only step that distinguishes the right key from a wrong one, so a
     failure here is reported as an authentication failure and nothing else. */
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)tag_len, (void *)tag) != 1) goto cleanup;
  if (EVP_DecryptFinal_ex(ctx, buf + out_len, &final_len) != 1) {
    st = UFLIB_SECRET_ERR_AUTH;
    goto cleanup;
  }

  out_len += final_len;
  buf[out_len] = '\0';
  result       = (char *)buf;
  buf          = NULL;
  st           = UFLIB_SECRET_OK;

cleanup:
  if (buf) {
    /* A failed tag means the bytes are not a plaintext, but they are derived
       from a secret and there is no reason to leave them in the heap. */
    UflibSecretWipe(buf, ct_len + 1);
    free(buf);
  }
  EVP_CIPHER_CTX_free(ctx);
  if (st == UFLIB_SECRET_OK) {
    *plaintext_out = result;
    if (plaintext_len_out) *plaintext_len_out = (size_t)out_len;
  }
  return st;
}

/*!
 * @brief AES-256-GCM encryption, emitting the ciphertext and its tag separately.
 */
static UflibSecretStatus sAes256GcmEncrypt(const unsigned char *key,
                                           const unsigned char *nonce, size_t nonce_len,
                                           const unsigned char *pt, size_t pt_len,
                                           unsigned char *ct_out, size_t *ct_len_out,
                                           unsigned char *tag_out, size_t tag_len)
{
  EVP_CIPHER_CTX *ctx       = NULL;
  int             out_len   = 0;
  int             final_len = 0;
  UflibSecretStatus st      = UFLIB_SECRET_ERR_AUTH;

  ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return UFLIB_SECRET_ERR_NO_MEMORY;

  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)nonce_len, NULL) != 1) goto cleanup;
  if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto cleanup;
  if (EVP_EncryptUpdate(ctx, ct_out, &out_len, pt, (int)pt_len) != 1) goto cleanup;
  if (EVP_EncryptFinal_ex(ctx, ct_out + out_len, &final_len) != 1) goto cleanup;

  out_len += final_len;
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)tag_len, tag_out) != 1) goto cleanup;

  *ct_len_out = (size_t)out_len;
  st          = UFLIB_SECRET_OK;

cleanup:
  EVP_CIPHER_CTX_free(ctx);
  return st;
}

PUBLIC_API UflibSecretStatus UflibSecretDecrypt(const char *envelope,
                                                const unsigned char key[UFLIB_SECRET_KEY_BYTES],
                                                char **plaintext_out, size_t *plaintext_len_out)
{
  if (!envelope || !*envelope || !key || !plaintext_out) return UFLIB_SECRET_ERR_INVALID_ARG;
  *plaintext_out = NULL;
  if (plaintext_len_out) *plaintext_len_out = 0;

  /* Not an envelope at all.  The reference passed this through unchanged; here
     it is a refusal, because a caller reaching this function has already said
     the value is encrypted and a pass-through would make that claim optional. */
  if (!UflibSecretHasPrefix(envelope)) return UFLIB_SECRET_ERR_NO_PREFIX;

  const char *payload = envelope + UFLIB_SECRET_PREFIX_LEN;

  /* The three components, split on the two colons that separate them.  A fourth
     colon means the ciphertext field holds something the scheme did not write. */
  const char *first = strchr(payload, ':');
  if (!first) return UFLIB_SECRET_ERR_MALFORMED;
  const char *second = strchr(first + 1, ':');
  if (!second) return UFLIB_SECRET_ERR_MALFORMED;
  if (strchr(second + 1, ':')) return UFLIB_SECRET_ERR_MALFORMED;

  size_t nonce_hex_len = (size_t)(first - payload);
  size_t ct_hex_len    = (size_t)(second - first - 1);

  char nonce_hex[UFLIB_SECRET_NONCE_BYTES * 2 + 1];
  char tag_hex[UFLIB_SECRET_TAG_BYTES * 2 + 1];

  if (nonce_hex_len != UFLIB_SECRET_NONCE_BYTES * 2) return UFLIB_SECRET_ERR_MALFORMED;
  if (strlen(second + 1) != UFLIB_SECRET_TAG_BYTES * 2) return UFLIB_SECRET_ERR_MALFORMED;

  memcpy(nonce_hex, payload, nonce_hex_len);
  nonce_hex[nonce_hex_len] = '\0';
  memcpy(tag_hex, second + 1, UFLIB_SECRET_TAG_BYTES * 2);
  tag_hex[UFLIB_SECRET_TAG_BYTES * 2] = '\0';

  /* The ciphertext field is copied out so it can be decoded without mutating the
     caller's string; it is a substring of the envelope, so it is not
     terminated where it ends. */
  char *ct_hex = (char *)malloc(ct_hex_len + 1);
  if (!ct_hex) return UFLIB_SECRET_ERR_NO_MEMORY;
  memcpy(ct_hex, first + 1, ct_hex_len);
  ct_hex[ct_hex_len] = '\0';

  unsigned char  nonce[UFLIB_SECRET_NONCE_BYTES];
  unsigned char  tag[UFLIB_SECRET_TAG_BYTES];
  unsigned char *ct      = NULL;
  size_t         ct_len  = 0;
  UflibSecretStatus st   = sHexDecodeFixed(nonce_hex, nonce, sizeof(nonce));

  if (st == UFLIB_SECRET_OK) st = sHexDecodeFixed(tag_hex, tag, sizeof(tag));
  if (st == UFLIB_SECRET_OK) st = sHexDecodeVarying(ct_hex, &ct, &ct_len);
  UflibSecretWipe(ct_hex, ct_hex_len);
  free(ct_hex);

  if (st == UFLIB_SECRET_OK) {
    st = sAes256GcmDecrypt(key, nonce, sizeof(nonce), ct, ct_len, tag, sizeof(tag),
                           plaintext_out, plaintext_len_out);
  }

  if (ct) {
    UflibSecretWipe(ct, ct_len);
    free(ct);
  }
  UflibSecretWipe(nonce, sizeof(nonce));
  UflibSecretWipe(tag, sizeof(tag));
  return st;
}

PUBLIC_API UflibSecretStatus UflibSecretEncrypt(const char *plaintext, size_t plaintext_len,
                                                const unsigned char key[UFLIB_SECRET_KEY_BYTES],
                                                char **envelope_out)
{
  if (!envelope_out) return UFLIB_SECRET_ERR_INVALID_ARG;
  *envelope_out = NULL;
  if (!key) return UFLIB_SECRET_ERR_INVALID_ARG;
  if (!plaintext && plaintext_len) return UFLIB_SECRET_ERR_INVALID_ARG;

  unsigned char  nonce[UFLIB_SECRET_NONCE_BYTES];
  unsigned char  tag[UFLIB_SECRET_TAG_BYTES];
  unsigned char *ct     = NULL;
  size_t         ct_len = 0;
  char *         result = NULL;

  if (sRandomBytes(nonce, sizeof(nonce)) != 0) return UFLIB_SECRET_ERR_IO;

  /* One byte of output per input byte at most, and GCM never exceeds that. */
  ct = (unsigned char *)malloc(plaintext_len ? plaintext_len : 1);
  if (!ct) return UFLIB_SECRET_ERR_NO_MEMORY;

  UflibSecretStatus st = sAes256GcmEncrypt(key, nonce, sizeof(nonce),
                                           (const unsigned char *)(plaintext ? plaintext : ""),
                                           plaintext_len, ct, &ct_len, tag, sizeof(tag));
  if (st != UFLIB_SECRET_OK) goto cleanup;

  {
    /* Lowercase, matching what the Python producer and every envelope already
       in service emit.  Decoding accepts either case, but a value written by
       this function should be indistinguishable from one written by the script. */
    size_t need = UFLIB_SECRET_PREFIX_LEN + sizeof(nonce) * 2 + 1 + ct_len * 2 + 1 + sizeof(tag) * 2 + 1;
    result      = (char *)malloc(need);
    if (!result) {
      st = UFLIB_SECRET_ERR_NO_MEMORY;
      goto cleanup;
    }

    char *p = result;
    memcpy(p, UFLIB_SECRET_PREFIX, UFLIB_SECRET_PREFIX_LEN);
    p += UFLIB_SECRET_PREFIX_LEN;

    bin2hex_case(nonce, sizeof(nonce), true, p);
    p += sizeof(nonce) * 2;
    *p++ = ':';

    if (ct_len) {
      bin2hex_case(ct, ct_len, true, p);
      p += ct_len * 2;
    }
    *p++ = ':';

    bin2hex_case(tag, sizeof(tag), true, p);
    p += sizeof(tag) * 2;
    *p = '\0';
  }

cleanup:
  if (ct) {
    UflibSecretWipe(ct, ct_len ? ct_len : 1);
    free(ct);
  }
  UflibSecretWipe(nonce, sizeof(nonce));
  UflibSecretWipe(tag, sizeof(tag));
  if (st != UFLIB_SECRET_OK) {
    if (result) free(result);
    return st;
  }
  *envelope_out = result;
  return UFLIB_SECRET_OK;
}

PUBLIC_API UflibSecretStatus UflibSecretGenerateKey(unsigned char key[UFLIB_SECRET_KEY_BYTES])
{
  if (!key) return UFLIB_SECRET_ERR_INVALID_ARG;
  if (sRandomBytes(key, UFLIB_SECRET_KEY_BYTES) != 0) return UFLIB_SECRET_ERR_IO;
  return UFLIB_SECRET_OK;
}

PUBLIC_API UflibSecretStatus UflibSecretKeyFromHex(const char *hex,
                                                   unsigned char key[UFLIB_SECRET_KEY_BYTES])
{
  if (!hex || !key) return UFLIB_SECRET_ERR_INVALID_ARG;
  if (strlen(hex) != UFLIB_SECRET_KEY_BYTES * 2) return UFLIB_SECRET_ERR_KEY_FORMAT;
  if (sHexDecodeFixed(hex, key, UFLIB_SECRET_KEY_BYTES) != UFLIB_SECRET_OK) {
    return UFLIB_SECRET_ERR_KEY_FORMAT;
  }
  return UFLIB_SECRET_OK;
}

/*!
 * @brief Reads at most @p cap bytes from @p path.
 *
 * Refuses anything larger rather than reading it, so a path that does not name a
 * key file cannot be turned into an unbounded allocation.
 */
static UflibSecretStatus sReadFileBounded(const char *path, unsigned char *out, size_t cap,
                                          size_t *got_out)
{
  FILE *f = fopen(path, "rb");
  if (!f) return UFLIB_SECRET_ERR_IO;

  size_t got = fread(out, 1, cap, f);
  if (ferror(f)) {
    fclose(f);
    return UFLIB_SECRET_ERR_IO;
  }

  /* One byte past the ceiling tells us the file is longer than a key. */
  unsigned char extra = 0;
  size_t        more  = fread(&extra, 1, 1, f);
  int           over  = (more == 1);
  fclose(f);

  if (over) return UFLIB_SECRET_ERR_KEY_FORMAT;
  *got_out = got;
  return UFLIB_SECRET_OK;
}

PUBLIC_API UflibSecretStatus UflibSecretKeyFromRawFile(const char *path,
                                                       unsigned char key[UFLIB_SECRET_KEY_BYTES])
{
  if (!path || !key) return UFLIB_SECRET_ERR_INVALID_ARG;

  /* Two bytes of headroom, so a key that arrives with a line ending is read
     whole and can be trimmed.  Sized to the key alone it could never hold the
     terminator, and the trim below would be both unreachable and out of bounds
     — which is what a 32-byte buffer made of it. */
  unsigned char buf[UFLIB_SECRET_KEY_BYTES + 2];
  size_t        got = 0;
  UflibSecretStatus st = sReadFileBounded(path, buf, sizeof(buf), &got);

  if (st == UFLIB_SECRET_OK) {
    /* A key file is raw bytes, but one written by an editor or echoed from a
       shell usually ends in a newline.  A key with a trailing newline appended
       is a different key, and the failure it produces is an authentication
       failure with nothing to point at the cause, so the terminator is trimmed
       here where the reason is still visible.  CR is stripped as well as LF, so
       a file that has been through a Windows editor is not a different key. */
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) got--;
    if (got != UFLIB_SECRET_KEY_BYTES) st = UFLIB_SECRET_ERR_KEY_FORMAT;
  }

  if (st == UFLIB_SECRET_OK) memcpy(key, buf, UFLIB_SECRET_KEY_BYTES);
  UflibSecretWipe(buf, sizeof(buf));
  return st;
}

PUBLIC_API UflibSecretStatus UflibSecretKeyFromBase64(const char *b64,
                                                      unsigned char key[UFLIB_SECRET_KEY_BYTES])
{
  if (!b64 || !key) return UFLIB_SECRET_ERR_INVALID_ARG;

  int            n     = 0;
  unsigned char *bytes = base64_decode((const unsigned char *)b64, (int)strlen(b64), &n);
  if (!bytes) return UFLIB_SECRET_ERR_KEY_FORMAT;

  UflibSecretStatus st = UFLIB_SECRET_ERR_KEY_FORMAT;
  if (n == UFLIB_SECRET_KEY_BYTES) {
    memcpy(key, bytes, UFLIB_SECRET_KEY_BYTES);
    st = UFLIB_SECRET_OK;
  }
  UflibSecretWipe(bytes, (size_t)(n > 0 ? n : 0));
  free(bytes);
  return st;
}

PUBLIC_API UflibSecretStatus UflibSecretKeyFromText(const char *text,
                                                    unsigned char key[UFLIB_SECRET_KEY_BYTES])
{
  if (!text || !key) return UFLIB_SECRET_ERR_INVALID_ARG;
  SHA256((const unsigned char *)text, strlen(text), key);
  return UFLIB_SECRET_OK;
}

/*!
 * @brief Strips leading and trailing whitespace in place, returning the start.
 *
 * The end of a line read by @c fgets already has its terminator removed here, so
 * a comment can be recognised and a key can be compared at its real length
 * rather than one that depends on the file's line endings.
 */
static char *sTrim(char *s)
{
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
    s[--n] = '\0';
  }
  return s;
}

/*!
 * @brief Appends an entry, or replaces the key of one already present.
 *
 * A repeated path keeps the first slot and takes the last key.  Refusing a
 * duplicate outright was the alternative; last-wins was chosen because a secrets
 * file assembled by concatenation is an ordinary way to build one, and the
 * later line is the one the author most recently meant.  The slot keeps its
 * original position so lookups are unaffected by where the override appeared.
 */
static UflibSecretStatus sFilePut(UflibSecretFile *f, const char *path,
                                  const unsigned char key[UFLIB_SECRET_KEY_BYTES])
{
  for (size_t i = 0; i < f->n; i++) {
    if (strcmp(f->slots[i].path, path) == 0) {
      memcpy(f->slots[i].key, key, UFLIB_SECRET_KEY_BYTES);
      return UFLIB_SECRET_OK;
    }
  }

  if (f->n == f->cap) {
    size_t           cap  = f->cap ? f->cap * 2 : 8;
    UflibSecretSlot *next = (UflibSecretSlot *)realloc(f->slots, cap * sizeof(*next));
    if (!next) return UFLIB_SECRET_ERR_NO_MEMORY;
    f->slots = next;
    f->cap   = cap;
  }

  f->slots[f->n].path = strdup(path);
  if (!f->slots[f->n].path) return UFLIB_SECRET_ERR_NO_MEMORY;
  memcpy(f->slots[f->n].key, key, UFLIB_SECRET_KEY_BYTES);
  f->n++;
  return UFLIB_SECRET_OK;
}

PUBLIC_API UflibSecretStatus UflibSecretFileOpen(const char *path, UflibSecretFile **out)
{
  if (!path || !*path || !out) return UFLIB_SECRET_ERR_INVALID_ARG;
  *out = NULL;

  FILE *fp = fopen(path, "r");
  if (!fp) return UFLIB_SECRET_ERR_IO;

  UflibSecretFile *f = (UflibSecretFile *)calloc(1, sizeof(*f));
  if (!f) {
    fclose(fp);
    return UFLIB_SECRET_ERR_NO_MEMORY;
  }
  f->path = strdup(path);
  if (!f->path) {
    fclose(fp);
    free(f);
    return UFLIB_SECRET_ERR_NO_MEMORY;
  }

  char              line[PRIV_UFLIB_SECRET_LINE_MAX];
  UflibSecretStatus st = UFLIB_SECRET_OK;

  while (fgets(line, sizeof(line), fp)) {
    /* A line filling the buffer without a terminator is longer than any real
       entry, and is refused rather than being read as a truncated one. */
    size_t len = strlen(line);
    if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
      st = UFLIB_SECRET_ERR_MALFORMED;
      break;
    }

    char *s = sTrim(line);
    if (*s == '\0' || *s == '#') continue;

    char *eq = strchr(s, '=');
    if (!eq) {
      st = UFLIB_SECRET_ERR_MALFORMED;
      break;
    }
    *eq = '\0';
    char *entry_path = sTrim(s);
    char *key_hex    = sTrim(eq + 1);
    if (*entry_path == '\0') {
      st = UFLIB_SECRET_ERR_MALFORMED;
      break;
    }

    unsigned char key[UFLIB_SECRET_KEY_BYTES];
    if (UflibSecretKeyFromHex(key_hex, key) != UFLIB_SECRET_OK) {
      UflibSecretWipe(key, sizeof(key));
      st = UFLIB_SECRET_ERR_MALFORMED;
      break;
    }
    st = sFilePut(f, entry_path, key);
    UflibSecretWipe(key, sizeof(key));
    if (st != UFLIB_SECRET_OK) break;
  }

  if (st == UFLIB_SECRET_OK && ferror(fp)) st = UFLIB_SECRET_ERR_IO;
  fclose(fp);

  if (st != UFLIB_SECRET_OK) {
    /* The line number is not reported through the status, which carries no room
       for it; the caller names the file, and a malformed file has few lines to
       look through. */
    UflibSecretFileClose(f);
    return st;
  }

  *out = f;
  return UFLIB_SECRET_OK;
}

PUBLIC_API const char *UflibSecretFilePath(const UflibSecretFile *f)
{
  return f ? f->path : NULL;
}

PUBLIC_API int UflibSecretFileHas(const UflibSecretFile *f, const char *path)
{
  if (!f || !path) return 0;
  for (size_t i = 0; i < f->n; i++) {
    if (strcmp(f->slots[i].path, path) == 0) return 1;
  }
  return 0;
}

PUBLIC_API UflibSecretStatus UflibSecretFileLookup(const UflibSecretFile *f, const char *path,
                                                   unsigned char key[UFLIB_SECRET_KEY_BYTES])
{
  if (!f || !path || !key) return UFLIB_SECRET_ERR_INVALID_ARG;
  for (size_t i = 0; i < f->n; i++) {
    if (strcmp(f->slots[i].path, path) == 0) {
      memcpy(key, f->slots[i].key, UFLIB_SECRET_KEY_BYTES);
      return UFLIB_SECRET_OK;
    }
  }
  return UFLIB_SECRET_ERR_NOT_FOUND;
}

PUBLIC_API void UflibSecretFileClose(UflibSecretFile *f)
{
  if (!f) return;
  for (size_t i = 0; i < f->n; i++) {
    UflibSecretWipe(f->slots[i].key, UFLIB_SECRET_KEY_BYTES);
    free(f->slots[i].path);
  }
  free(f->slots);
  free(f->path);
  free(f);
}
