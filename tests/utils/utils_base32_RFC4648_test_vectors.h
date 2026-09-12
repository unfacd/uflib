//
// Created by ayman on 3/09/2026.
//

/**
 * @file utils_base32_test_vectors.h
 * @brief utils_base32_test_vectors
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
 * The blocker: RFC 4648 vectors vs. a Crockford module

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
 *
 */

#ifndef UFLIB_UTILS_BASE32_TEST_VECTORS_H
#define UFLIB_UTILS_BASE32_TEST_VECTORS_H

/* ============================================================
 * base32_adversarial.h — RFC 4648 Base32 adversarial test maps
 *
 * Two tables:
 *   b32_strict_tests[]  — strict decoder contract (above)
 *   b32_lenient_tests[] — lenient decoder contract (above)
 *
 * Every entry: encoded input -> expected bytes OR required failure.
 * Entries whose outcome is a genuine policy decision are marked
 * "policy" in the note — decide, then flip expect if needed.
 *
 * Compile the decoder under test with -fsanitize=address,undefined.
 * The \xFF / \x80 inputs exist specifically to turn a signed-char
 * lookup-table index into an ASan out-of-bounds report.
 * ============================================================ */

#include <stddef.h>

typedef enum {
    B32_OK         = 0,  /* must succeed; output must match byte-for-byte   */
    B32_FAIL_ANY   = 1,  /* must fail (any error)                            */
    B32_FAIL_CHAR  = 2,  /* must fail; ideally reports: bad character        */
    B32_FAIL_PAD   = 3,  /* must fail; ideally reports: padding error        */
    B32_FAIL_LEN   = 4,  /* must fail; ideally reports: impossible length    */
    B32_FAIL_CANON = 5   /* must fail; ideally reports: non-canonical bits   */
} b32_expect;            /* classes 2..5 are advisory unless you map codes   */

typedef struct {
    const char          *enc;     /* input; MAY embed '\0' -> always use enc_len */
    size_t               enc_len; /* true length (sizeof-1); strlen may differ   */
    const unsigned char *dec;     /* expected bytes; "" when failure expected    */
    size_t               dec_len;
    b32_expect           expect;
    const char          *note;
} b32_test;

#define B32T(e, d, x, n) \
    { (e), sizeof(e)-1, (const unsigned char *)(d), sizeof(d)-1, (x), (n) }

/* shared expected blobs (octal escapes: unambiguous, max 3 digits) */
#define B32_SWEEP_OUT "\000D2\024\307BT\2665\317\204e:V\327\306u\276w\357"
#define B32_000_0FF   "\000\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017"

/* ====================================================================
 * MAP 1 — STRICT
 * Every B32_OK entry must also re-encode canonically to *exactly* the
 * input string (bijectivity / round-trip check).
 * ==================================================================== */
static const b32_test b32_strict_tests[] = {
/* ---- RFC 4648 section 8 known-answer vectors ---- */
B32T("",                 "",                     B32_OK, "empty in -> empty out"),
B32T("MY======",         "f",                    B32_OK, "1 byte  -> 2 chars + 6 '='"),
B32T("MZXQ====",         "fo",                   B32_OK, "2 bytes -> 4 + 4"),
B32T("MZXW6===",         "foo",                  B32_OK, "3 bytes -> 5 + 3"),
B32T("MZXW6YQ=",         "foob",                 B32_OK, "4 bytes -> 7 + 1"),
B32T("MZXW6YTB",         "fooba",                B32_OK, "5 bytes -> 8 + 0"),
B32T("MZXW6YTBOI======", "foobar",               B32_OK, "6 bytes -> 10 + 6"),

/* ---- decoded output is hostile to C strings ---- */
B32T("AA======",         "\000",                 B32_OK, "output IS a NUL: length must come from decoder, never strlen"),
B32T("AAAA====",         "\000\000",             B32_OK, "two NULs"),
B32T("AAAAA===",         "\000\000\000",         B32_OK, "three NULs"),
B32T("AAAAAAA=",         "\000\000\000\000",     B32_OK, "four NULs"),
B32T("AAAAAAAA",         "\000\000\000\000\000", B32_OK, "five NULs"),
B32T("AAAAAAAB",         "\000\000\000\000\001", B32_OK, "NUL-heavy, nonzero tail"),
B32T("HU6T2PI=",         "====",                 B32_OK, "decoded output *is* '=' characters"),
B32T("BI======",         "\n",                   B32_OK, "newline in output"),
B32T("JQ======",         "L",                    B32_OK, "0x4C in output"),
B32T("EJOA====",         "\"\\",                 B32_OK, "quote + backslash in output"),

/* ---- 0xFF / high-bit output bytes ---- */
B32T("74======",         "\377",                 B32_OK, "0xFF output"),
B32T("777Q====",         "\377\377",             B32_OK, ""),
B32T("77776===",         "\377\377\377",         B32_OK, "note: 5 data chars, not 4"),
B32T("777777Y=",         "\377\377\377\377",     B32_OK, "canonical 4-byte 0xFF (Y=11000, leftover 000)"),
B32T("77777777",         "\377\377\377\377\377", B32_OK, "5 bytes, no padding"),
B32T("7777777A",         "\377\377\377\377\340", B32_OK, "0xFF..0xE0"),

/* ---- multi-quantum / buffer boundaries ---- */
B32T("MZXW6YTBMZXW6YTBMZXW6YTB", "foobafoobafooba", B32_OK, "3 quanta, no padding (15 bytes)"),
B32T("MZXW6YTBMZXW6YTBMZXW6YTBMZXW6YTBMZXW6YTBMZXW6YTBMZXW6YTBMZXW6YTBMZXW6YTB",
                         "foobafoobafoobafoobafoobafoobafoobafooba",
                                                 B32_OK, "64 chars -> 40 bytes: off-by-one output-buffer bait"),
B32T("AAAAAAAAAAAAAAAA", "\000\000\000\000\000\000\000\000\000\000", B32_OK, "16 chars -> 10 zero bytes"),
B32T("AAAQEAYEAUDAOCAJBIFQYDIOB4======", B32_000_0FF,
                                                 B32_OK, "bytes 0x00..0x0F: every bit-pattern class"),
B32T("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", B32_SWEEP_OUT,
                                                 B32_OK, "all 32 alphabet chars exactly once; output bytes 0x84..0xEF stress signed handling"),

/* ---- FAIL: impossible lengths ---- */
B32T("A",                "", B32_FAIL_LEN, "1 data char = 5 bits, no whole byte"),
B32T("ABC",              "", B32_FAIL_LEN, "3 mod 8"),
B32T("ABCDEF",           "", B32_FAIL_LEN, "6 mod 8"),
B32T("MYQ======",        "", B32_FAIL_LEN, "9 chars total: not a multiple of 8"),
B32T("MZXW6YTBO======",  "", B32_FAIL_LEN, "15 chars total"),
B32T("A=======",         "", B32_FAIL_LEN, "1 data + 7 '='"),
B32T("ABC=====",         "", B32_FAIL_LEN, "3 data + 5 '='"),
B32T("ABCDEF==",         "", B32_FAIL_LEN, "6 data + 2 '='"),
B32T("AAAAAAAAAAAAAAAAA","", B32_FAIL_LEN, "17 chars: off-by-one on % 8"),

/* ---- FAIL: padding ---- */
B32T("MY",               "", B32_FAIL_PAD, "missing padding (lenient impls decode 'f')"),
B32T("MZXQ",             "", B32_FAIL_PAD, "missing padding"),
B32T("MZXW6",            "", B32_FAIL_PAD, ""),
B32T("MZXW6YQ",          "", B32_FAIL_PAD, ""),
B32T("MZXW6YTBOI",       "", B32_FAIL_PAD, "10 data, no padding"),
B32T("AAAAA",            "", B32_FAIL_PAD, ""),
B32T("AAAAAAA",          "", B32_FAIL_PAD, ""),
B32T("MZXW6YTB========", "", B32_FAIL_PAD, "8 data chars need 0 '='; 8 supplied"),
B32T("MY==============", "", B32_FAIL_PAD, "2 data, 14 '=' — pad-run loops must not over-read"),
B32T("========",         "", B32_FAIL_PAD, "padding only, no data"),
B32T("=",                "", B32_FAIL_PAD, "lone '='"),
B32T("MY==MY==",         "", B32_FAIL_PAD, "'=' mid-stream"),
B32T("M=XW6YTBOI======", "", B32_FAIL_PAD, "'=' before data"),
B32T("MZXW6YTBOI=====X", "", B32_FAIL_PAD, "data after '=' inside final quantum"),
B32T("MZXQ====MZXQ====", "", B32_FAIL_PAD, "data after padding; quantum/streaming decoders return 'fofo' (policy)"),
B32T("MZXW6YTBOI======MZXW6YTB", "", B32_FAIL_PAD, "data after padding"),
B32T("MZXW6YTBOI======MZXW6YTBOI======", "", B32_FAIL_PAD,
     "two concatenated streams: stop-at-'=' decoders silently yield 'foobar' and drop half the input"),

/* ---- FAIL: characters ---- */
B32T("mzxw6ytboi======", "", B32_FAIL_CHAR, "lowercase (policy: RFC alphabet is A-Z2-7; flip if you case-fold)"),
B32T("MzXw6YtBoI======", "", B32_FAIL_CHAR, "mixed case"),
B32T("MzXw6YtBoi",       "", B32_FAIL_CHAR, "mixed case, unpadded"),
B32T("abcdefghijklmnopqrstuvwxyz234567", "", B32_FAIL_CHAR, "full lowercase alphabet"),
B32T("M0XW6YTBOI======", "", B32_FAIL_CHAR, "'0' not in alphabet (O/0 confusion)"),
B32T("M1XW6YTBOI======", "", B32_FAIL_CHAR, "'1' not in alphabet (I/l/1 confusion)"),
B32T("MZXW6YTBO0======", "", B32_FAIL_CHAR, "'0' again"),
B32T("MZXW6YTBO8======", "", B32_FAIL_CHAR, "'8' not in alphabet"),
B32T("MZXW6YTBO9======", "", B32_FAIL_CHAR, "'9' not in alphabet"),
B32T("MZXW6YTBOl======", "", B32_FAIL_CHAR, "lowercase L looks like I; case-folding decoders return 'foobar' here"),
B32T("MZXW6YTBOi======", "", B32_FAIL_CHAR, "lowercase i; case-folding decoders return 'foobar' here"),
B32T(" MZXW6YTBOI======", "", B32_FAIL_CHAR, "leading space"),
B32T("MZXW6YTBOI====== ", "", B32_FAIL_CHAR, "trailing space"),
B32T("MZXW6 YTBOI======", "", B32_FAIL_CHAR, "interior space"),
B32T("MZXW6\tYTBOI======", "", B32_FAIL_CHAR, "interior tab"),
B32T("MZXW6\r\nYTBOI======", "", B32_FAIL_CHAR, "interior CRLF"),
B32T("MZXW6YTBOI======\n",   "", B32_FAIL_CHAR, "trailing newline: the #1 real-world bug"),
B32T("MZXW6YTBOI======\r\n", "", B32_FAIL_CHAR, "trailing CRLF"),
B32T(" ",                "", B32_FAIL_CHAR, "whitespace only"),
B32T("\r\n",             "", B32_FAIL_CHAR, "CRLF only"),
B32T("\t",               "", B32_FAIL_CHAR, "tab only"),
B32T("\xEF\xBB\xBFMZXW6YTBOI======", "", B32_FAIL_CHAR, "UTF-8 BOM prefix (high-bit bytes)"),
B32T("MZXW6YTBOI======\xE2\x80\x8B", "", B32_FAIL_CHAR, "trailing U+200B zero-width space"),
B32T("MZXW6YTBOI======\xC3\xA9",     "", B32_FAIL_CHAR, "trailing UTF-8 'e-acute'"),
B32T("MZXW\xFFYTBOI======", "", B32_FAIL_CHAR, "0xFF byte: signed-char table index -> negative -> OOB read (run under ASan)"),
B32T("\x80\x81\x82\x83\x84\x85\x86\x87", "", B32_FAIL_CHAR, "eight >= 0x80 bytes"),
B32T("CPNMUOJ1E8======", "", B32_FAIL_CHAR, "base32hex encoding of 'foobar': '1' invalid here — alphabet confusion"),
B32T("CSQPYRKE8",        "", B32_FAIL_CHAR, "Crockford base32 of 'foobar': '8' invalid (also 9 data chars)"),
B32T("c3zs6aubqe",       "", B32_FAIL_CHAR, "z-base-32 of 'foobar'"),

/* ---- FAIL: non-canonical (nonzero leftover bits) ---- */
B32T("MZ======",         "", B32_FAIL_CANON, "decodes to 'f' if trailing bits ignored (Z=11001, leftover 01)"),
B32T("M2======",         "", B32_FAIL_CANON, "leftover 10 -> 'f'"),
B32T("M3======",         "", B32_FAIL_CANON, "leftover 11 -> 'f'"),
B32T("MZX7====",         "", B32_FAIL_CANON, "-> 'fo'"),
B32T("MZXW7===",         "", B32_FAIL_CANON, "-> 'foo'"),
B32T("MZXW6YTR=",        "", B32_FAIL_CANON, "-> 'foob'"),
B32T("MZXW6YTS=",        "", B32_FAIL_CANON, "-> 'foob'"),
B32T("MZXW6YTT=",        "", B32_FAIL_CANON, "-> 'foob'"),
B32T("MZXW6YTBOJ======", "", B32_FAIL_CANON, "-> 'foobar': one-bit-off character (J vs I)"),
B32T("MZXW6YTBOK======", "", B32_FAIL_CANON, "-> 'foobar'"),
B32T("MZXW6YTBOL======", "", B32_FAIL_CANON, "-> 'foobar'"),
B32T("7777777=",         "", B32_FAIL_CANON, "-> 4x0xFF"),
B32T("7777====",         "", B32_FAIL_CANON, "-> 2x0xFF"),
B32T("77777===",         "", B32_FAIL_CANON, "-> 3x0xFF"),
B32T("AAAB====",         "", B32_FAIL_CANON, "-> 2x0x00 (low bits set inside a 'zero' block)"),

/* ---- FAIL: C-string / API traps ---- */
B32T("MZ\0W6YTBOI======",  "", B32_FAIL_ANY, "embedded NUL: strlen decoders see 'MZ' (fail as bad pad); (ptr,len) decoders see the NUL as a bad char. Both fail — for different reasons."),
B32T("MZXW6YTBOI======\0", "", B32_FAIL_ANY, "trailing NUL: strlen decoders succeed with 'foobar'; length-aware decoders must reject. Pick a contract, adjust expect."),
};
#define B32_STRICT_COUNT  (sizeof b32_strict_tests  / sizeof b32_strict_tests[0])

/* ====================================================================
 * MAP 2 — LENIENT
 * Contract: case-insensitive; ignore \t \n \v \f \r ' ' anywhere;
 * strip one trailing '=' run (any length); drop leftover bits (<8);
 * length after stripping must be 0,2,4,5 or 7 mod 8; all else errors.
 * ==================================================================== */
static const b32_test b32_lenient_tests[] = {
/* ---- canonical + case + optional padding ---- */
B32T("",                 "",        B32_OK, "empty"),
B32T("MY======",         "f",       B32_OK, "canonical"),
B32T("mY======",         "f",       B32_OK, "mixed case"),
B32T("my======",         "f",       B32_OK, "lowercase"),
B32T("MZXQ====",         "fo",      B32_OK, ""),
B32T("MZXW6===",         "foo",     B32_OK, ""),
B32T("MZXW6YQ=",         "foob",    B32_OK, ""),
B32T("MZXW6YTB",         "fooba",   B32_OK, ""),
B32T("MZXW6YTBOI======", "foobar",  B32_OK, ""),
B32T("MZXW6YTBOI",       "foobar",  B32_OK, "padding omitted"),
B32T("mzxw6ytboi======", "foobar",  B32_OK, "lowercase"),
B32T("mzxw6ytboi",       "foobar",  B32_OK, "lowercase, no padding"),
B32T("MzXw6YtBoI======", "foobar",  B32_OK, "mixed case"),
B32T("mZXW6ytboI",       "foobar",  B32_OK, "mixed case, no padding"),
B32T("MY",               "f",       B32_OK, "no padding"),
B32T("my",               "f",       B32_OK, ""),
B32T("MZXQ",             "fo",      B32_OK, ""),
B32T("mZXq",             "fo",      B32_OK, ""),
B32T("MZXW6",            "foo",     B32_OK, ""),
B32T("MZXW6YQ",          "foob",    B32_OK, ""),
B32T("MY=",              "f",       B32_OK, "1 '=' instead of 6"),
B32T("MY==",             "f",       B32_OK, "2 '='"),
B32T("MY=====",          "f",       B32_OK, "5 '='"),
B32T("MY=======",        "f",       B32_OK, "7 '=': OK only if the whole trailing '=' run is stripped (policy)"),
B32T("MY==============", "f",       B32_OK, "14 '=': pad-stripping loop must not over-read/overflow (policy: strip-all)"),

/* ---- non-canonical leftover bits silently dropped ---- */
B32T("MZ======",         "f",       B32_OK, "leftover 01 dropped"),
B32T("M2======",         "f",       B32_OK, "leftover 10 dropped"),
B32T("M3======",         "f",       B32_OK, "leftover 11 dropped"),
B32T("MZX7====",         "fo",      B32_OK, ""),
B32T("MZXW7===",         "foo",     B32_OK, ""),
B32T("MZXW6YTR=",        "foob",    B32_OK, ""),
B32T("MZXW6YTBOJ======", "foobar",  B32_OK, "J vs I differ only in dropped bits"),
B32T("MZXW6YTBOl======", "foobar",  B32_OK, "lowercase l -> L: case fold + dropped bits"),
B32T("MZXW6YTBOi======", "foobar",  B32_OK, "lowercase i -> I"),
B32T("7777777=",         "\377\377\377\377", B32_OK, ""),
B32T("7777====",         "\377\377",         B32_OK, ""),
B32T("77777===",         "\377\377\377",     B32_OK, ""),
B32T("AAAB====",         "\000\000",         B32_OK, ""),

/* ---- NUL / 0xFF outputs, case-folded ---- */
B32T("AA======",         "\000",    B32_OK, "NUL output — never strlen the result"),
B32T("aa======",         "\000",    B32_OK, ""),
B32T("AA",               "\000",    B32_OK, "no padding"),
B32T("aaaa",             "\000\000", B32_OK, ""),
B32T("aaaaa",            "\000\000\000", B32_OK, ""),
B32T("aaaaaaa",          "\000\000\000\000", B32_OK, ""),
B32T("aaaaaaaa",         "\000\000\000\000\000", B32_OK, ""),
B32T("77777777",         "\377\377\377\377\377", B32_OK, ""),
B32T("777777y=",         "\377\377\377\377", B32_OK, "lowercase y"),
B32T("74======",         "\377",    B32_OK, ""),
B32T("hu6t2pi=",         "====",    B32_OK, "output is '=' chars"),
B32T("bi======",         "\n",      B32_OK, ""),
B32T("jq======",         "L",       B32_OK, ""),

/* ---- alphabet sweeps ---- */
B32T("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", B32_SWEEP_OUT, B32_OK, "all 32 chars once"),
B32T("abcdefghijklmnopqrstuvwxyz234567", B32_SWEEP_OUT, B32_OK, "lowercase sweep"),
B32T("AAAQEAYEAUDAOCAJBIFQYDIOB4======", B32_000_0FF,  B32_OK, "bytes 00..0F"),
B32T("aaaqeayeaudaocajbifqydiob4",       B32_000_0FF,  B32_OK, "lowercase, unpadded"),

/* ---- whitespace tolerance ---- */
B32T("MZXW6YTBOI======\n",      "foobar", B32_OK, "trailing newline"),
B32T("MZXW6YTBOI======\r\n",    "foobar", B32_OK, "trailing CRLF"),
B32T("MZXW6YTBOI======\n\n\n",  "foobar", B32_OK, ""),
B32T("MZXW6YTBOI======\t",      "foobar", B32_OK, "trailing tab"),
B32T(" MZXW6YTBOI====== ",      "foobar", B32_OK, "surrounding spaces"),
B32T("\tMZXW6\tYTBOI\t======\t", "foobar", B32_OK, "tabs everywhere"),
B32T("MZXW6 YTBOI======",       "foobar", B32_OK, "interior space"),
B32T("MZXW6\r\nYTBOI\r\n======", "foobar", B32_OK, "MIME-style folding"),
B32T("MZXW6\nYTBOI\n======",    "foobar", B32_OK, "LF folding"),
B32T("MZXW6YTB\r\nMZXQ====",    "foobafo", B32_OK, "line break mid-stream; padding only on the last line"),
B32T("MZXW6YTB\r\nMZXW6YTB\r\nMZXQ====", "foobafoobafo", B32_OK, "three lines"),
B32T("MZXW6YTBOI\r\nMZXW6YTB",  "foobarfooba", B32_OK, "line break, no padding"),
B32T("   ",               "",        B32_OK, "whitespace only -> empty"),
B32T("\r\n",             "",        B32_OK, ""),
B32T("\t \r\n ",         "",        B32_OK, ""),
B32T("\fMZXW6YTBOI\v======", "foobar", B32_OK, "form feed + vertical tab: does your whitespace set include \\f\\v?"),
B32T("m\nz\tx\r w\n6\ty t b o i", "foobar", B32_OK, "whitespace + case folding + no padding"),
B32T("M Z X W 6 Y T B O I = = = = = =", "foobar", B32_OK, "space between every character"),

/* ---- FAIL: impossible even when lenient ---- */
B32T("A",                "", B32_FAIL_LEN, "1 char = 5 leftover bits; some impls drop to empty — decide policy"),
B32T("ABC",              "", B32_FAIL_LEN, "7 leftover bits"),
B32T("ABCDEF",           "", B32_FAIL_LEN, "6 leftover bits"),
B32T("MZXW6YTBO",        "", B32_FAIL_LEN, "9 data: 1 mod 8"),
B32T("MYQ======",        "", B32_FAIL_LEN, "3 data after pad strip"),
B32T("ABC=====",         "", B32_FAIL_LEN, ""),
B32T("ABCDEF==",         "", B32_FAIL_LEN, ""),
B32T("AAAAAAAAAAAAAAAAA","", B32_FAIL_LEN, "17: 1 mod 8"),

/* ---- FAIL: padding abuse ---- */
B32T("=",                "", B32_FAIL_ANY, "no data at all; strip-all-pads impls return empty+OK — decide policy"),
B32T("========",         "", B32_FAIL_ANY, "ditto"),
B32T("MY==MY==",         "", B32_FAIL_PAD, "'=' mid-stream; stop-at-first-'=' decoders return 'f' — decide policy"),
B32T("MZXQ====MZXQ====", "", B32_FAIL_PAD, "stop-at-'=' -> 'fo'; quantum decoders -> 'fofo' — decide policy"),
B32T("MZXW6YTBOI======MZXW6YTBOI======", "", B32_FAIL_PAD, "quantum decoders -> 'foobarfoobar' — decide policy"),
B32T("M=XW6YTBOI======", "", B32_FAIL_PAD, "'=' before data"),
B32T("MZXW6YTBOI=====X", "", B32_FAIL_PAD, "data after '='"),

/* ---- FAIL: characters ---- */
B32T("M0XW6YTBOI======", "", B32_FAIL_CHAR, "'0' invalid even case-folded"),
B32T("M1XW6YTBOI======", "", B32_FAIL_CHAR, "'1'"),
B32T("MZXW6YTBO8======", "", B32_FAIL_CHAR, "'8' (Crockford/z-base-32 only)"),
B32T("MZXW6YTBO9======", "", B32_FAIL_CHAR, "'9' (z-base-32 only)"),
B32T("m1======",         "", B32_FAIL_CHAR, ""),
B32T("m0======",         "", B32_FAIL_CHAR, ""),
B32T("mzxw6ytbo1",       "", B32_FAIL_CHAR, "'1' after case fold"),
B32T("mzxw6ytbo0",       "", B32_FAIL_CHAR, "'0' after case fold"),
B32T("mzxw6ytbo8",       "", B32_FAIL_CHAR, "'8'"),
B32T("\xEF\xBB\xBFmzxw6ytboi", "", B32_FAIL_CHAR, "UTF-8 BOM; some impls skip it — policy"),
B32T("MZXW\xFFYTBOI======", "", B32_FAIL_CHAR, "0xFF in input (ASan the lookup table)"),
B32T("\x80\x81\x82\x83\x84\x85\x86\x87", "", B32_FAIL_CHAR, ">= 0x80 bytes"),
B32T("CPNMUOJ1E8======", "", B32_FAIL_CHAR, "base32hex 'foobar' ('1')"),
B32T("CSQPYRKE8",        "", B32_FAIL_CHAR, "Crockford 'foobar' ('8')"),
B32T("c3zs6aubqe",       "", B32_FAIL_CHAR, "z-base-32 'foobar' ('3' invalid after fold)"),

/* ---- FAIL: API traps ---- */
B32T("\0",                     "", B32_FAIL_ANY, "1-byte NUL input: strlen decoders see empty (OK); (ptr,len) must reject"),
B32T("MZ\0W6YTBOI======",      "", B32_FAIL_ANY, "strlen decoders see 'MZ' -> 'f' (would pass); length-aware must reject"),
B32T("MZXW6YTBOI======\0",     "", B32_FAIL_ANY, "strlen decoders: OK 'foobar'; length-aware: reject — pick per API"),
};
#define B32_LENIENT_COUNT (sizeof b32_lenient_tests / sizeof b32_lenient_tests[0])


#endif //UFLIB_UTILS_BASE32_TEST_VECTORS_H
