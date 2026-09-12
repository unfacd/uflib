/**
 * @file utils_base32_test_standalone.c
 * @brief utils_base32_test_standalone
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
 *The blocker: RFC 4648 vectors vs. a Crockford module

tests/utils/utils_base32_test_vectors.h encodes RFC 4648 Base32 (A-Z2-7, = padding), but utils_base32.c / base32.h implement Douglas Crockford Base32 (0123456789ABCDEFGHJKMNPQRSTVWXYZ, unpadded, I/L/O/U excluded). These are different alphabets with different value mappings, so the tables cannot be fed to base32dec() and expected to pass.

Concrete, byte-level evidence:

1. The header self-identifies as RFC 4648 — utils_base32_test_vectors.h:25 ("RFC 4648 Base32 adversarial test maps") and :73 ("RFC 4648 section 8 known-answer vectors").
2. Same bytes, contradictory encodings. Crockford encodes "foobar" → "CSQPYRK1E8" (committed in base32.h:59 and utils_base32_tests.cpp:165). The vector table instead asserts "MZXW6YTBOI======" → "foobar" (:80) — that's the RFC 4648 encoding.
3. Byte math confirms it. RFC 4648 "MY" = M(12)Y(24) → 01100 11000 → 0x66 = "f", matching :75. Under Crockford, "M"=20 and "Y"=30 → 10100 11110 → 0xA7, not "f". Crockford's encoding of "f" is "CR", and ==== padding is something Crockford never emits at all.
4. FAIL_CHAR entries are wrong for Crockford. Vectors :149-153 mark '0', '1', '8', '9' as invalid ("not in alphabet") — but all four are valid Crockford digits (the alphabet literally starts 0123456789).
5. The lenient contract diverges. The header's lenient map (:198-203) promises whitespace-skipping, leftover-bit dropping, and mod-8 length validation — RFC 4648 semantics. Crockford lenient (base32.h:73-85) instead does I/L→1, O→0, - skip, = terminator, and does not skip whitespace.

So "converting" the vectors isn't a mechanical re-map either — Crockford is unpadded, so all the B32_FAIL_PAD cases, the 8-char-quantum structure, and the mod-8 length checks lose their meaning and would need a semantic rewrite.

Secondary blockers (even after the alphabet question is settled)

- utils_base32_test_standalone.c has no main(), and its b32_decode_fn signature (int (*)(const unsigned char *in, size_t in_len, unsigned char *out, size_t *out_len)) does not match base32dec(void *dest, size_t dest_len, const char *src, bool is_lenient) — so it can't drive the real API.
- Neither file is registered in tests/utils/CMakeLists.txt (only utils_base32_tests.cpp is; the base64 standalone is the template).
- Both files carry CRLF line endings (git warns they'll be re-normalized), and the .h is static-table data, so it needs an include guard strategy when compiled into a .c harness.
 */

/* ====================================================================
 * Optional harness
 * ==================================================================== */
#include <stdio.h>
#include <string.h>

/* Decoder contract under test:
 *   returns 0 on success, nonzero on failure;
 *   *out_len is capacity on entry, bytes written on success. */
typedef int (*b32_decode_fn)(const unsigned char *in, size_t in_len, unsigned char *out, size_t *out_len);

static void b32_hex(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n && i < 24; i++) printf("%02x ", p[i]);
    if (n > 24) printf("...");
}

static int b32_run_table(const char *label, const b32_test *tv, size_t n, b32_decode_fn dec)
{
    int fails = 0;
    unsigned char out[256];   /* largest dec_len in the tables is 40 */

    for (size_t i = 0; i < n; i++) {
        size_t olen = sizeof out;
        int rc = dec((const unsigned char *)tv[i].enc, tv[i].enc_len, out, &olen);
        int ok = (tv[i].expect == B32_OK)
               ? (rc == 0 && olen == tv[i].dec_len &&
                  (olen == 0 || memcmp(out, tv[i].dec, olen) == 0))
               : (rc != 0);

        if (!ok) {
            fails++;
            printf("FAIL %s[%zu] expect=%s\n  in  : ", label, i,
                   tv[i].expect == B32_OK ? "OK" : "FAIL");
            b32_hex((const unsigned char *)tv[i].enc, tv[i].enc_len);
            if (rc == 0) { printf("\n  got : "); b32_hex(out, olen); }
            printf("\n  note: %s\n", tv[i].note);
        }

        /* Boundary pass (capacity-aware APIs only — delete if yours
         * has no capacity parameter, or adjust if it needs room for
         * a NUL terminator): exact fit must succeed, one-short must
         * fail WITHOUT writing past the buffer. */
        if (tv[i].expect == B32_OK && tv[i].dec_len > 0 &&
            tv[i].dec_len < sizeof out) {
            size_t olen2 = tv[i].dec_len;
            int rc2 = dec((const unsigned char *)tv[i].enc, tv[i].enc_len,
                          out, &olen2);
            if (rc2 != 0 || olen2 != tv[i].dec_len) {
                fails++; printf("FAIL %s[%zu] exact-fit buffer rejected\n",
                                label, i);
            }
            olen2 = tv[i].dec_len - 1;
            if (dec((const unsigned char *)tv[i].enc, tv[i].enc_len,
                    out, &olen2) == 0) {
                fails++; printf("FAIL %s[%zu] accepted a 1-byte-short buffer\n",
                                label, i);
            }
        }
    }
    printf("%s: %zu vectors, %d failure%s\n\n", label, n, fails,
           fails == 1 ? "" : "s");
    return fails;
}