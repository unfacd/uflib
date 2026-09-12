/**
 * @file utils_base64url.c
 * @brief Base64url (RFC 4648 §5) encode/decode — URL- and filename-safe.
 *
 * Shares the single base64 encode/decode core with utils_base64.c (see
 * utils_base64_priv.h): identical algorithm, with characters 62/63 changed to
 * '-' and '_' and '=' padding omitted, exactly as RFC 4648 §5 "base64url".
 *
 * Historical note: this module was originally derived from Apache APR's
 * apr_base64.c and the cran/base64url package.  That duplicated algorithm has
 * been replaced by the shared core, so no third-party code remains here.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <uflib/utils_base64url.h>

#include "utils_base64_priv.h"

#include <limits.h>

size_t
base64_decoded_size(const unsigned char *buf, size_t len)
{
  if (buf == NULL) {
    return 0;
  }

  // A valid unpadded base64url input of `len` bytes decodes to exactly
  // (3*len)/4 bytes (the partial final group contributes one byte per pair),
  // plus one byte for the NUL terminator that base64url_decode() always writes.
  // This closed form is exact for valid input and a safe upper bound for
  // invalid input, and it matches the capacity bound enforced by
  // base64url_decode().
  return (3 * len) / 4 + 1;
}

ssize_t
base64url_decode(const unsigned char *data, size_t len,
                 unsigned char *result_out, size_t result_cap)
{
  int count = 0;

  if (data == NULL || result_out == NULL) {
    return -1;
  }

  // The shared core takes an int length; refuse inputs too large to represent
  // (the same bound as the standard base64_decode API).
  if (len > (size_t)INT_MAX) {
    return -1;
  }

  // A fully-valid input of `len` bytes decodes to at most (3*len)/4 bytes, plus
  // the NUL terminator.  Enforce the capacity up front so a mis-sized buffer
  // cannot be overrun.
  if (result_cap < (3 * len) / 4 + 1) {
    return -2;
  }

  if (!sBase64DecodeCore(data, (int)len, result_out, &count, &kUflibBase64Url, 0, 0)) {
    return -1;
  }

  return (ssize_t)count;
}

void
base64url_encode(const unsigned char *data, size_t data_sz, unsigned char *result_out)
{
  if (result_out == NULL) {
    return;
  }

  if (data == NULL) {
    *result_out = '\0';
    return;
  }

  (void)sBase64EncodeCore(data, data_sz, result_out, &kUflibBase64Url);
}
