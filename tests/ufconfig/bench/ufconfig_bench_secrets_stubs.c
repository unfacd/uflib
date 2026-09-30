/**
 * @file ufconfig_bench_secrets_stubs.c
 * @brief Secrets entry points the config module calls, stubbed for the benchmark.
 *
 * Companion to @ref ufconfig_bench_stubs.c, and present for the same reason: the
 * benchmark compiles the module's sources rather than linking the library so it
 * can measure them at -O2 with sanitizers off, and pulling in the real
 * `utils_secrets.c` would drag OpenSSL, libbsd and libresolv into a measurement
 * of the configuration module.  The secrets code is not on any path the
 * benchmark measures — its schema declares no encrypted field, so
 * `UfConfigCreate` never opens a secrets file and no value is ever decrypted.
 *
 * Only `UflibSecretFileClose` is reached at all, from `UfConfigDestroy`, and it
 * is reached with NULL.
 *
 * ## If an encrypted field is ever added to this benchmark's schema
 *
 * These stubs must go, and the real translation unit must be linked instead.
 * Every one of them returns failure or nothing, so an encrypted field would make
 * `UfConfigCreate` fail rather than silently pass — except
 * @ref UflibSecretWipe, which would silently not wipe.  The stub is a no-op
 * because a real wipe over a buffer this translation unit never owns is not
 * something a stub can do correctly, and the alternative is a stub that looks
 * like it provides a guarantee it does not.
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

#include <uflib/utils_secrets.h>

PUBLIC_API const char *UflibSecretStatusString(UflibSecretStatus s)
{
  (void)s;
  return "STUBBED";
}

PUBLIC_API void UflibSecretWipe(void *p, size_t n)
{
  (void)p;
  (void)n;
}

PUBLIC_API UflibSecretStatus UflibSecretDecrypt(const char *envelope,
                                                const unsigned char key[UFLIB_SECRET_KEY_BYTES],
                                                char **plaintext_out, size_t *plaintext_len_out)
{
  (void)envelope;
  (void)key;
  (void)plaintext_out;
  (void)plaintext_len_out;
  return UFLIB_SECRET_ERR_AUTH;
}

PUBLIC_API UflibSecretStatus UflibSecretFileOpen(const char *path, UflibSecretFile **out)
{
  (void)path;
  if (out) *out = NULL;
  return UFLIB_SECRET_ERR_IO;
}

PUBLIC_API const char *UflibSecretFilePath(const UflibSecretFile *f)
{
  (void)f;
  return NULL;
}

PUBLIC_API int UflibSecretFileHas(const UflibSecretFile *f, const char *path)
{
  (void)f;
  (void)path;
  return 0;
}

PUBLIC_API UflibSecretStatus UflibSecretFileLookup(const UflibSecretFile *f, const char *path,
                                                   unsigned char key[UFLIB_SECRET_KEY_BYTES])
{
  (void)f;
  (void)path;
  (void)key;
  return UFLIB_SECRET_ERR_NOT_FOUND;
}

PUBLIC_API void UflibSecretFileClose(UflibSecretFile *f)
{
  (void)f;
}
