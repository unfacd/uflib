//
// Created by ayman on 3/09/2026.
//

/**
 * @file utilis_base64_test_vectors.h
 * @brief utilis_base64_test_vectors
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
 * @note
* gtest: 34 pass, 1 fail (LenientTestVectors); ASan/UBSan clean. The lenient decoder correctly handles whitespace, URL-safe chars, missing padding, and over-padding/garbage.

The 2 failures are vector bugs, not decoder bugs

Both fail on memcmp, and I verified the decoder's output is actually the correct base64:

┌──────────────────────────┬────────────────┬────────────────┬───────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│          Vector          │  expected_hex  │ Correct decode │                                                  Why it's wrong                                                   │
├──────────────────────────┼────────────────┼────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ "A-B-C" → "\x00\x10\x83" │ 0x00 0x10 0x83 │ 0x03 0xE0 0x7E │ A-B-C (→A+B+C = 0,62,1,62,2) decodes to 03 E0 7E. 00 10 83 is actually "ABCD".                                    │
├──────────────────────────┼────────────────┼────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ "aA==" → "\x00"          │ 0x00           │ 0x68           │ Base64 is case-sensitive: 'a'=26 ≠ 'A'=0, so "aA==" → 0x68; "AA==" → 0x00. "case insensitive" is a misconception. │
└──────────────────────────┴────────────────┴────────────────┴───────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

These are in utilis_base64_test_vectors.h (your file), so I left them untouched. Two options to make the suite fully green:

1. Fix the vectors — "A-B-C"'s expected_hex → "\x03\xe0\x7e" (or change encoded to "ABCD"), and "aA==" → either encoded = "AA==" or expected_hex = "\x68".
2. Leave them as-is as a deliberate record of the header's bugs.

The following fixes were applied to above:

┌─────────┬────────────────────────────────────────────────┬───────────────────────────────────────────────┐
│ Vector  │                     Before                     │                     After                     │
├─────────┼────────────────────────────────────────────────┼───────────────────────────────────────────────┤
│ "A-B-C" │ expected_hex = "\x00\x10\x83"                  │ "\x03\xe0\x7e"                                │
├─────────┼────────────────────────────────────────────────┼───────────────────────────────────────────────┤
│ "aA=="  │ expected_hex = "\x00", desc "case insensitive" │ "\x68", desc "Lowercase input (a-z alphabet)" │
└─────────┴────────────────────────────────────────────────┴───────────────────────────────────────────────┘
 *
 */

#ifndef UFLIB_UTILIS_BASE64_TEST_VECTORS_H
#define UFLIB_UTILIS_BASE64_TEST_VECTORS_H

typedef struct {
    const char *encoded;
    const char *expected_hex; // Use memcmp against this
    bool should_pass;
    const char *description;
} Base64TestVector;

Base64TestVector strict_tests[] = {
    // --- BOUNDARY LENGTHS (Testing padding rules: ==, =, none) ---
    {
        .encoded = "",
        .expected_hex = "",
        .should_pass = true,
        .description = "Empty string"
    },
    {
        .encoded = "AA==",
        .expected_hex = "\x00",
        .should_pass = true,
        .description = "1 byte input (exact 0x00 boundary, requires ==)"
    },
    {
        .encoded = "AAA=",
        .expected_hex = "\x00\x00",
        .should_pass = true,
        .description = "2 byte input (exact 0x00 boundary, requires =)"
    },
    {
        .encoded = "AAAA",
        .expected_hex = "\x00\x00\x00",
        .should_pass = true,
        .description = "3 byte input (no padding required)"
    },
    {
        .encoded = "/w==",
        .expected_hex = "\xff",
        .should_pass = true,
        .description = "1 byte input (exact 0xFF boundary, requires ==)"
    },
    {
        .encoded = "//8=",
        .expected_hex = "\xff\xff",
        .should_pass = true,
        .description = "2 byte input (exact 0xFF boundary, requires =)"
    },
    {
        .encoded = "////",
        .expected_hex = "\xff\xff\xff",
        .should_pass = true,
        .description = "3 byte input (all 1s, no padding)"
    },

    // --- ALPHABET EXTREMES ---
    {
        .encoded = "AQID",
        .expected_hex = "\x01\x02\x03",
        .should_pass = true,
        .description = "Lowest non-zero values"
    },
    {
        .encoded = "AP8=",
        .expected_hex = "\x00\xff",
        .should_pass = true,
        .description = "High bit transition (0x00 to 0xFF)"
    },

    // --- STRICT ADVERSARIAL (MUST FAIL) ---
    {
        .encoded = "/w+=",
        .expected_hex = "",
        .should_pass = false,
        .description = "INVALID: Non-zero padding bits. '+' is 111110. Last 4 bits should be 0000."
    },
    {
        .encoded = "AA",
        .expected_hex = "",
        .should_pass = false,
        .description = "INVALID: Missing padding for 1-byte block"
    },
    {
        .encoded = "AAA",
        .expected_hex = "",
        .should_pass = false,
        .description = "INVALID: Missing padding for 2-byte block"
    },
    {
        .encoded = "AAAA=",
        .expected_hex = "",
        .should_pass = false,
        .description = "INVALID: Extraneous padding on 3-byte block"
    },
    {
        .encoded = "A AA==",
        .expected_hex = "",
        .should_pass = false,
        .description = "INVALID: Strict decoders reject whitespace"
    },
    {
        .encoded = "A!A=",
        .expected_hex = "",
        .should_pass = false,
        .description = "INVALID: Illegal character '!'"
    }
};

Base64TestVector lenient_tests[] = {
    // --- MISSING PADDING (Extremely common in JWTs and APIs) ---
    {
        .encoded = "/w",
        .expected_hex = "\xff",
        .should_pass = true,
        .description = "Missing '==' padding"
    },
    {
        .encoded = "//8",
        .expected_hex = "\xff\xff",
        .should_pass = true,
        .description = "Missing '=' padding"
    },
    {
        .encoded = "TWFu",
        .expected_hex = "Man",
        .should_pass = true,
        .description = "Standard 'Man' with padding stripped"
    },

    // --- WHITESPACE GARBAGE (MIME encoding) ---
    {
        .encoded = "T W F u",
        .expected_hex = "Man",
        .should_pass = true,
        .description = "Spaces interspersed"
    },
    {
        .encoded = "\n\tTWFu\n\t",
        .expected_hex = "Man",
        .should_pass = true,
        .description = "Leading/trailing tabs and newlines"
    },
    {
        .encoded = "TWF\r\nu",
        .expected_hex = "Man",
        .should_pass = true,
        .description = "Windows CRLF line endings in the middle"
    },

    // --- URL-SAFE VARIANT ---
    {
        .encoded = "__8=",
        .expected_hex = "\xff\xff",
        .should_pass = true,
        .description = "URL-safe characters (_ instead of /)"
    },
    {
        .encoded = "A-B-C",
        .expected_hex = "\x03\xe0\x7e",
        .should_pass = true,
        .description = "URL-safe characters (- instead of +)"
    },
    {
        .encoded = "aA==",
        .expected_hex = "\x68",
        .should_pass = true,
        .description = "Lowercase input (a-z alphabet)"
    },

    // --- LENIENT ADVERSARIAL (The "Please don't crash" tests) ---
    {
        .encoded = "  /w  ", // Missing padding, surrounded by spaces
        .expected_hex = "\xff",
        .should_pass = true,
        .description = "Missing padding AND whitespace"
    },
    {
        .encoded = "TWFu===garbage_data",
        .expected_hex = "Man",
        .should_pass = true,
        .description = "Over-padded with trailing garbage (lenient should stop at 'u')"
    },
    {
        .encoded = "//8\n", // Missing padding, trailing newline
        .expected_hex = "\xff\xff",
        .should_pass = true,
        .description = "Missing padding with trailing newline"
    }
};
#endif //UFLIB_UTILIS_BASE64_TEST_VECTORS_H
