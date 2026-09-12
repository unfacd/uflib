/**
 * @file utils_base64_test_standalone.c
 * @brief Standalone harness driving uflib's base64_decode against the shared test vectors.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 3/09/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include <uflib/utils_base64.h>
#include "utilis_base64_test_vectors.h"

// Number of decoded bytes for a base64 string: count the data characters
// (skipping whitespace, stopping at '=') and scale by 3/4.  `expected_hex` is
// raw bytes (it may contain embedded NULs), so its length cannot be taken with
// strlen.
static size_t
expected_decoded_len(const char *b64)
{
  size_t d = 0;
  for (const char *p = b64; *p != '\0'; ++p) {
    if (*p == '=') {
      break;
    }
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f') {
      continue;
    }
    ++d;
  }
  return (d * 3) / 4;
}

typedef unsigned char *(*DecodeFn)(const unsigned char *, int, int *);

// 1 = decoded and matched, 0 = decoded but mismatched, -1 = rejected.
static int
decode_vector(const Base64TestVector *t, DecodeFn decode)
{
  int ret = -1;
  unsigned char *out = decode(
      (const unsigned char *)t->encoded, (int)strlen(t->encoded), &ret);

  if (out == NULL) {
    return -1;
  }

  const size_t want_len = expected_decoded_len(t->encoded);
  const bool ok = ((size_t)ret == want_len) &&
                  (memcmp(out, t->expected_hex, want_len) == 0);
  free(out);
  return ok ? 1 : 0;
}

static int
run_tests(Base64TestVector *tests, int count, const char *category, DecodeFn decode)
{
  int passed = 0;

  for (int i = 0; i < count; ++i) {
    Base64TestVector *t = &tests[i];
    const int r = decode_vector(t, decode);
    const bool actually_passed = (r == 1);

    if (actually_passed == t->should_pass) {
      ++passed;
    } else {
      printf("FAIL [%s]: %s\n", category, t->description);
      printf("  Input:    \"%s\"\n", t->encoded);
      if (t->should_pass) {
        printf("  Expected: decode succeeded; decoder %s\n",
               (r == -1) ? "rejected" : "mismatched");
      } else {
        printf("  Expected: FAILURE, but decoder succeeded.\n");
      }
    }
  }

  printf("\n[%s] Passed: %d / %d\n\n", category, passed, count);
  return count - passed;
}

int
main(void)
{
  const int strict_count = (int)(sizeof(strict_tests) / sizeof(strict_tests[0]));
  const int lenient_count = (int)(sizeof(lenient_tests) / sizeof(lenient_tests[0]));

  int failures = 0;
  failures += run_tests(strict_tests, strict_count, "STRICT", base64_decode);
  failures += run_tests(lenient_tests, lenient_count, "LENIENT", base64_decode_lenient);

  return (failures > 0) ? 1 : 0;
}
