/**
 * @file utils_base64url_tests.cpp
 * @brief Adversarial test suite for the utils_base64url module (RFC 4648 §5).
 *
 * Public API (utils_base64url.h / src/utils_base64url.c):
 *
 *   - void    base64url_encode(const unsigned char *data, size_t data_sz, unsigned char *result_out)
 *   - ssize_t base64url_decode(const unsigned char *data, size_t len, unsigned char *result_out, size_t result_cap)
 *   - size_t  base64_decoded_size(const unsigned char *buf, size_t len)
 *
 * The module shares a single encode/decode core with utils_base64.c.  This
 * suite judges the URL-safe variant against an independent reference
 * encoder/decoder (the oracle), RFC 4648 §5 known vectors, exhaustive
 * round-trips over every byte value, and the full strict-contract truth table
 * from dev/technical_designs/UFLIB_TEST_BASE64URL_STRATEGY.md (which mandates
 * `(pointer, length)` inputs so embedded NULs are rejected, not truncated).
 *
 * Hostile cases that previously exposed real flaws:
 *
 *   - base64url_encode() once read past a 0- or 1-byte input (size_t underflow
 *     in its loop bound) — regressed here by encoding empty and single-byte
 *     buffers.
 *   - base64url_decode() once took a NUL-terminated string and silently
 *     discarded invalid characters — now a `(ptr, len)` decoder that returns -1
 *     on any non-alphabet byte, '=' padding, '+'/'/', whitespace, an embedded
 *     NUL, a lone trailing character, or non-canonical trailing bits.
 *   - base64_decoded_size() once over-reported the decoded size — now it must
 *     equal decoded_length + 1 exactly.
 *   - write bounds are verified with canary bytes around the output buffer, so
 *     an overrun is caught even when the value itself happens to look correct.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "gtest/gtest.h"

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/types.h>

extern "C" {
#include "uflib/utils_base64.h"
#include "uflib/utils_base64url.h"
}

namespace {

// RFC 4648 §5 URL-safe alphabet.
static const char kUrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// ── Independent reference oracle ──────────────────────────────────────────

// Reference unpadded URL-safe encoder.  This is the oracle; a disagreement with
// base64url_encode is a finding, not an adjustment of the oracle.
std::string
EncodeUrlRef(const unsigned char *bytes, size_t n)
{
  std::string out;
  size_t i = 0;
  while (i + 3 <= n) {
    uint32_t v = (static_cast<uint32_t>(bytes[i]) << 16) |
                 (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                 static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kUrlAlphabet[(v >> 18) & 0x3Fu]);
    out.push_back(kUrlAlphabet[(v >> 12) & 0x3Fu]);
    out.push_back(kUrlAlphabet[(v >> 6) & 0x3Fu]);
    out.push_back(kUrlAlphabet[v & 0x3Fu]);
    i += 3;
  }
  const size_t rem = n - i;
  if (rem == 1) {
    uint32_t v = static_cast<uint32_t>(bytes[i]) << 16;
    out.push_back(kUrlAlphabet[(v >> 18) & 0x3Fu]);
    out.push_back(kUrlAlphabet[(v >> 12) & 0x3Fu]);
  } else if (rem == 2) {
    uint32_t v = (static_cast<uint32_t>(bytes[i]) << 16) |
                 (static_cast<uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kUrlAlphabet[(v >> 18) & 0x3Fu]);
    out.push_back(kUrlAlphabet[(v >> 12) & 0x3Fu]);
    out.push_back(kUrlAlphabet[(v >> 6) & 0x3Fu]);
  }
  return out;
}

// Reference strict URL-safe decoder.  Returns false (and leaves `out` empty) on
// any non-alphabet byte, a lone trailing character, or non-canonical trailing
// bits.
bool
DecodeUrlRef(const std::string &s, std::vector<unsigned char> &out)
{
  out.clear();
  uint32_t acc = 0;
  int nbits = 0;
  for (unsigned char c : s) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-') v = 62;
    else if (c == '_') v = 63;
    else return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    nbits += 6;
    if (nbits >= 8) {
      nbits -= 8;
      out.push_back(static_cast<unsigned char>((acc >> nbits) & 0xFFu));
      acc &= (1u << nbits) - 1u;
    }
  }
  if (s.size() % 4 == 1) return false;      // lone trailing character
  if (nbits > 0 && acc != 0) return false;  // non-canonical trailing bits
  return true;
}

// Decode a (ptr, len) buffer through base64url_decode() into a vector.
ssize_t
DecodeUrl(const unsigned char *data, size_t len, std::vector<unsigned char> &out)
{
  std::vector<unsigned char> buf(base64_decoded_size(data, len), 0xAA);
  const ssize_t n = base64url_decode(data, len, buf.data(), buf.size());
  if (n >= 0) {
    out.assign(buf.begin(), buf.begin() + n);
  } else {
    out.clear();
  }
  return n;
}

// Convenience overload for NUL-terminated inputs.
ssize_t
DecodeUrl(const char *s, std::vector<unsigned char> &out)
{
  return DecodeUrl(reinterpret_cast<const unsigned char *>(s), strlen(s), out);
}

// Exact unpadded encoded length for n input bytes: ceil(4n/3) = (4n + 2) / 3.
size_t
EncodedLen(size_t n)
{
  return (4 * n + 2) / 3;
}

// ── Encoding ──────────────────────────────────────────────────────────────

TEST(Base64UrlEncode, MatchesReferenceAllLengths)
{
  unsigned char in[80];
  for (size_t i = 0; i < sizeof(in); ++i)
    in[i] = static_cast<unsigned char>((i * 37 + 11) & 0xFF);

  for (size_t n = 0; n <= sizeof(in); ++n) {
    std::string want = EncodeUrlRef(in, n);
    unsigned char out[128] = {0};
    base64url_encode(in, n, out);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(out)), want) << "len " << n;
    EXPECT_EQ(strlen(reinterpret_cast<char *>(out)), want.size()) << "len " << n;
  }
}

TEST(Base64UrlEncode, KnownRfc4648Vectors)
{
  struct { const char *in; size_t len; const char *want; } v[] = {
    {"",         0, ""},
    {"f",        1, "Zg"},
    {"fo",       2, "Zm8"},
    {"foo",      3, "Zm9v"},
    {"foob",     4, "Zm9vYg"},
    {"fooba",    5, "Zm9vYmE"},
    {"foobar",   6, "Zm9vYmFy"},
    {"\xFB",     1, "-w"},   // 6-bit value 62 -> '-'
    {"\xFF",     1, "_w"},   // 6-bit value 63 -> '_'
  };
  for (const auto &e : v) {
    unsigned char out[32] = {0};
    base64url_encode(reinterpret_cast<const unsigned char *>(e.in), e.len, out);
    EXPECT_STREQ(reinterpret_cast<char *>(out), e.want);
  }
}

TEST(Base64UrlEncode, EverySixBitValueMapsToCorrectCharacter)
{
  // Encoding { v<<2, 0, 0 } puts 6-bit value v into the first output char.
  for (int v = 0; v < 64; ++v) {
    unsigned char in[3] = {static_cast<unsigned char>(v << 2), 0, 0};
    unsigned char out[8] = {0};
    base64url_encode(in, 3, out);
    EXPECT_EQ(out[0], static_cast<unsigned char>(kUrlAlphabet[v])) << "v=" << v;
  }
}

TEST(Base64UrlEncode, UnpaddedAndExactLength)
{
  // Expected encoded length is ceil(4n/3) (no padding), plus the NUL.  The URL
  // alphabet must never emit '=', '+', or '/'.
  for (size_t n = 0; n < 32; ++n) {
    unsigned char in[32] = {0};
    unsigned char out[64] = {0};
    base64url_encode(in, n, out);
    EXPECT_EQ(strlen(reinterpret_cast<char *>(out)), EncodedLen(n)) << "n=" << n;
    EXPECT_EQ(strchr(reinterpret_cast<char *>(out), '='), nullptr) << "n=" << n;
    EXPECT_EQ(strchr(reinterpret_cast<char *>(out), '+'), nullptr) << "n=" << n;
    EXPECT_EQ(strchr(reinterpret_cast<char *>(out), '/'), nullptr) << "n=" << n;
  }
}

TEST(Base64UrlEncode, EmptyAndSingleByteRegression)
{
  // Regression: the old loop bound (i < data_sz - 2) underflowed for data_sz
  // 0 and 1, reading past the input buffer.
  unsigned char out[16] = {0};

  base64url_encode(nullptr, 0, out);
  EXPECT_STREQ(reinterpret_cast<char *>(out), "");

  const unsigned char one = 0x66;  // 'f'
  base64url_encode(&one, 1, out);
  EXPECT_STREQ(reinterpret_cast<char *>(out), "Zg");
}

TEST(Base64UrlEncode, NullDataGuarded)
{
  unsigned char out[16] = {0xAA, 0xBB};

  base64url_encode(nullptr, 0, out);
  EXPECT_EQ(out[0], '\0');

  out[0] = 0xAA;
  base64url_encode(nullptr, 5, out);  // NULL data with non-zero size
  EXPECT_EQ(out[0], '\0');

  base64url_encode(reinterpret_cast<const unsigned char *>("x"), 1, nullptr);
  // must not crash; nothing to assert beyond surviving the call
}

TEST(Base64UrlEncode, ExhaustiveRoundTripAllByteValues)
{
  unsigned char out[16] = {0};
  unsigned char dec[8] = {0};
  for (int b = 0; b < 256; ++b) {
    const unsigned char byte = static_cast<unsigned char>(b);
    base64url_encode(&byte, 1, out);
    const ssize_t n = base64url_decode(out, 2, dec, sizeof dec);
    ASSERT_EQ(n, 1) << "b=" << b;
    EXPECT_EQ(dec[0], byte) << "b=" << b;
  }
}

// ── Decoding ──────────────────────────────────────────────────────────────

TEST(Base64UrlDecode, KnownRfc4648Vectors)
{
  struct { const char *in; const char *want; size_t len; } v[] = {
    {"",          "",        0},
    {"Zg",        "f",       1},
    {"Zm8",       "fo",      2},
    {"Zm9v",      "foo",     3},
    {"Zm9vYg",    "foob",    4},
    {"Zm9vYmE",   "fooba",   5},
    {"Zm9vYmFy",  "foobar",  6},
  };
  for (const auto &e : v) {
    std::vector<unsigned char> got;
    const ssize_t n = DecodeUrl(e.in, got);
    ASSERT_EQ(n, static_cast<ssize_t>(e.len)) << e.in;
    EXPECT_EQ(std::string(got.begin(), got.end()), e.want) << e.in;
  }
}

TEST(Base64UrlDecode, RoundTripAllLengths)
{
  unsigned char in[300];
  for (size_t i = 0; i < sizeof(in); ++i)
    in[i] = static_cast<unsigned char>((i * 97 + 5) & 0xFF);

  unsigned char enc[512] = {0};
  for (size_t n = 0; n <= sizeof(in); ++n) {
    base64url_encode(in, n, enc);
    std::vector<unsigned char> got;
    const ssize_t m = DecodeUrl(enc, EncodedLen(n), got);
    ASSERT_EQ(m, static_cast<ssize_t>(n)) << "n=" << n;
    EXPECT_EQ(got.size(), n) << "n=" << n;
    if (n) EXPECT_EQ(memcmp(got.data(), in, n), 0) << "n=" << n;
  }
}

TEST(Base64UrlDecode, MatchesReferenceDecoder)
{
  // Any string over the alphabet (plus hostile stragglers) must decode to the
  // same result as the reference oracle.
  const char *cases[] = {
    "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy",
    "-w", "_w", "----", "____", "-_-_", "9__-",
    "Z", "Zm", "Zh",                    // lone / non-canonical trailing bits
    "Zg!", "!Zg", "Zg=", "Z g", "Zg\n", // hostile bytes
    "Zm9v+", "Zm9v/", "Zg==",           // std alphabet / padding
  };
  for (const char *s : cases) {
    std::vector<unsigned char> got;
    const ssize_t n = DecodeUrl(s, got);

    std::vector<unsigned char> ref;
    const bool ref_ok = DecodeUrlRef(s, ref);

    if (ref_ok) {
      ASSERT_GE(n, 0) << s;
      EXPECT_EQ(got, ref) << s;
    } else {
      EXPECT_EQ(n, -1) << s;
    }
  }
}

TEST(Base64UrlDecode, RejectsInvalidAlphabet)
{
  const char *bad[] = {
    "!!!!",      // all invalid
    "Zg!!",      // invalid mid/tail
    "!Zg",       // invalid head
    "Zm9v!",     // invalid tail after complete group
    " Zm9v",     // leading space
    "Zm9v ",     // trailing space
    "Zm\n9v",    // embedded newline
    "Zm9v\t",    // tab
    "Zm9v\x01",  // control byte
    "Zm9v.",     // punctuation
  };
  unsigned char out[64] = {0};
  for (const char *s : bad) {
    EXPECT_EQ(base64url_decode(reinterpret_cast<const unsigned char *>(s),
                               strlen(s), out, sizeof out), -1)
        << "input: " << s;
  }
}

TEST(Base64UrlDecode, RejectsPaddingAndStdAlphabet)
{
  unsigned char out[64] = {0};
  EXPECT_EQ(base64url_decode((const unsigned char *)"Zg==", 4, out, sizeof out), -1);
  EXPECT_EQ(base64url_decode((const unsigned char *)"Zm9v", 4, out, sizeof out), 3);
  EXPECT_EQ(base64url_decode((const unsigned char *)"Zm9v+", 5, out, sizeof out), -1);
  EXPECT_EQ(base64url_decode((const unsigned char *)"Zm9v/", 5, out, sizeof out), -1);
  EXPECT_EQ(base64url_decode((const unsigned char *)"Zm9v=", 5, out, sizeof out), -1);
}

TEST(Base64UrlDecode, RejectsNonCanonicalTrailingBits)
{
  unsigned char out[64] = {0};
  EXPECT_EQ(base64url_decode((const unsigned char *)"Zg", 2, out, sizeof out), 1);
  EXPECT_EQ(base64url_decode((const unsigned char *)"Zh", 2, out, sizeof out), -1);
  EXPECT_EQ(base64url_decode((const unsigned char *)"Z", 1, out, sizeof out), -1);
}

// Mirror of the strict-base64url truth table in
// dev/technical_designs/UFLIB_TEST_BASE64URL_STRATEGY.md §2.  Every accept
// row was derived by hand from the 6-bit group layout and cross-checked against
// two independent decoders, so a disagreement here is a finding.

TEST(Base64UrlDecode, StrategyTruthTableMustDecode)
{
  struct V { const char *enc; const std::vector<unsigned char> bytes; };
  const std::vector<V> v = {
    {"", {}},
    {"AA", {0x00}}, {"AQ", {0x01}}, {"Ag", {0x02}}, {"Aw", {0x03}},
    {"Zg", {0x66}}, {"Zm8", {0x66, 0x6f}}, {"Zm9v", {0x66, 0x6f, 0x6f}},
    {"Zm9vYg", {0x66, 0x6f, 0x6f, 0x62}},
    {"Zm9vYmE", {0x66, 0x6f, 0x6f, 0x62, 0x61}},
    {"Zm9vYmFy", {0x66, 0x6f, 0x6f, 0x62, 0x61, 0x72}},
    {"AAA", {0x00, 0x00}}, {"AAE", {0x00, 0x01}},
    {"AAAA", {0x00, 0x00, 0x00}},
    {"AAAAAA", {0x00, 0x00, 0x00, 0x00}},
    {"AAAAAAA", {0x00, 0x00, 0x00, 0x00, 0x00}},
    {"ag", {0x6a}},                 // case trap: 'Ag' -> 0x02, 'ag' -> 0x6a
    {"QUJDRA", {0x41, 0x42, 0x43, 0x44}},
    {"l1I0", {0x97, 0x52, 0x34}},   // 'l'/'1'/'I'/'0' are distinct
    {"0OIl", {0xd0, 0xe2, 0x25}},   // 'O'/'0', 'I'/'l' distinct
    {"A-B_", {0x03, 0xe0, 0x7f}},   // both URL-safe specials
    {"-w", {0xfb}}, {"_w", {0xff}}, // specials in a 2-char quantum
    {"-_8", {0xfb, 0xff}}, {"_-w", {0xff, 0xec}},
    {"-_-_", {0xfb, 0xff, 0xbf}},
  };
  for (const auto &e : v) {
    std::vector<unsigned char> got;
    const ssize_t n = DecodeUrl(e.enc, got);
    ASSERT_EQ(n, static_cast<ssize_t>(e.bytes.size())) << e.enc;
    EXPECT_EQ(got, e.bytes) << e.enc;
  }
}

TEST(Base64UrlDecode, StrategyTruthTableMustReject)
{
  // B: non-canonical trailing bits; C: len%4==1; D: '=' padding;
  // E: standard-alphabet '+'/'/'; F: whitespace / UTF-8 / raw bytes.
  const char *bad[] = {
    "AB", "AC", "AP", "aa", "ME", "A-", "A_", "-_", "_-", "Zh", "Zm9",
    "AAB", "AAD", "AAF", "-_-", "_-_", "AAAAAB", "AAAAAAB",
    "A", "AAAAA", "Zm9vA", "AAAAAAAAA",
    "=", "==", "===", "====", "AA=", "AA==", "Zg==", "Zm8=", "Zm9vYg==",
    "QUJDRA==", "=AAA", "A===", "AA=A", "A=A", "AAAA=", "AAAA==",
    "AAAA===", "AAAA====", "AA==AA",
    "+/+/", "A+B/", "AA+A", "AA/A", "A+AA", "/AAA", "AAA+", "AAA/", "AB+/",
    "AA ", " AA", "A A", "AA\t", "AA\n", "AA\r\n", "Zm9v ",
    "\xEF\xBB\xBF" "AA",            // UTF-8 BOM
    "AA\xE2\x80\x8B",               // zero-width space
    "AA\xC2\xA0",                   // no-break space
    "\xD0\x90" "A",                 // Cyrillic А
    "\xEF\xBC\xA1" "A",             // fullwidth Ａ
    "\xEF\xBC\x8D" "w",             // fullwidth －
    "\xE2\x88\x92" "w",             // minus −
    "\xE2\x80\x93" "w",             // en dash –
    "\xC2\xAD" "w",                 // soft hyphen (U+00AD, C2 AD)
    "\xEF\xBC\xBF" "w",             // fullwidth ＿
    "\x80\x81\x82", "\xff",         // raw bytes outside the alphabet
  };
  unsigned char out[64] = {0};
  for (const char *s : bad) {
    EXPECT_EQ(base64url_decode(reinterpret_cast<const unsigned char *>(s),
                               strlen(s), out, sizeof out), -1)
        << "input: " << s;
  }
}

TEST(Base64UrlDecode, RejectsEmbeddedNul)
{
  // (ptr, len) inputs: an embedded NUL is an ordinary byte and must be rejected
  // rather than silently truncating the input — the "strlen trap".
  const unsigned char in_aa_nul[]    = {'A', 'A', 0x00};
  const unsigned char in_nul_aa[]    = {0x00, 'A', 'A'};
  const unsigned char in_aa_nul_bb[] = {'A', 'A', 0x00, 'B', 'B'};
  unsigned char out[64] = {0};
  EXPECT_EQ(base64url_decode(in_aa_nul, 3, out, sizeof out), -1);
  EXPECT_EQ(base64url_decode(in_nul_aa, 3, out, sizeof out), -1);
  EXPECT_EQ(base64url_decode(in_aa_nul_bb, 5, out, sizeof out), -1);
}

TEST(Base64UrlDecode, CapacityTooSmallReturnsMinusTwo)
{
  const unsigned char in[] = "Zm9v";  // "foo" -> 3 bytes
  unsigned char out[4] = {0};
  EXPECT_EQ(base64url_decode(in, 4, out, 3), -2);   // 3 < (3*4)/4 + 1
  EXPECT_EQ(base64url_decode(in, 4, out, 4), 3);    // exact fit
  EXPECT_EQ(memcmp(out, "foo", 3), 0);
}

TEST(Base64UrlDecode, NullArguments)
{
  unsigned char out[8] = {0};
  EXPECT_EQ(base64url_decode(nullptr, 0, out, sizeof out), -1);
  EXPECT_EQ(base64url_decode(reinterpret_cast<const unsigned char *>("Zm9v"),
                             4, nullptr, 8), -1);
}

// ── base64_decoded_size() ─────────────────────────────────────────────────

TEST(Base64UrlDecodedSize, EqualsDecodedLengthPlusNul)
{
  const char *cases[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE",
                         "Zm9vYmFy", "-w", "_w", "----", "____"};
  for (const char *s : cases) {
    std::vector<unsigned char> got;
    const ssize_t n = DecodeUrl(s, got);
    ASSERT_GE(n, 0) << s;
    EXPECT_EQ(base64_decoded_size(reinterpret_cast<const unsigned char *>(s),
                                  strlen(s)),
              static_cast<size_t>(n) + 1) << s;
  }
}

TEST(Base64UrlDecodedSize, NullAndEmpty)
{
  EXPECT_EQ(base64_decoded_size(nullptr, 0), 0u);
  EXPECT_EQ(base64_decoded_size(reinterpret_cast<const unsigned char *>(""), 0),
            1u);  // room for the NUL terminator
}

// ── Write-footprint canaries ──────────────────────────────────────────────

TEST(Base64UrlWriteBounds, EncodeFitsInExactBuffer)
{
  // Allocate exactly EncodedLen(n)+1 bytes and flank with canaries so an
  // overrun/underrun is caught even when the value looks right.
  unsigned char in[64];
  for (size_t i = 0; i < sizeof(in); ++i)
    in[i] = static_cast<unsigned char>(i * 3 + 1);

  for (size_t n = 0; n <= sizeof(in); ++n) {
    const size_t need = EncodedLen(n) + 1;  // encoded chars + NUL
    std::vector<unsigned char> buf(need + 16, 0);
    unsigned char *out = buf.data() + 8;
    out[-1] = 0xA5;                 // canary before
    out[need] = 0x5A;               // canary after (index need)

    base64url_encode(in, n, out);

    EXPECT_EQ(out[-1], 0xA5) << "underrun n=" << n;
    EXPECT_EQ(out[need], 0x5A) << "overrun n=" << n;
    EXPECT_EQ(out[strlen(reinterpret_cast<char *>(out))], '\0') << "n=" << n;
  }
}

TEST(Base64UrlWriteBounds, DecodeFitsInDecodedSizeBuffer)
{
  unsigned char in[96];
  for (size_t i = 0; i < sizeof(in); ++i)
    in[i] = static_cast<unsigned char>(i * 13 + 7);

  unsigned char enc[256] = {0};
  for (size_t n = 0; n <= sizeof(in); ++n) {
    base64url_encode(in, n, enc);
    const size_t enclen = EncodedLen(n);
    const size_t need = base64_decoded_size(enc, enclen);
    std::vector<unsigned char> buf(need + 16, 0);
    unsigned char *out = buf.data() + 8;
    out[-1] = 0xA5;
    out[need] = 0x5A;

    const ssize_t m = base64url_decode(enc, enclen, out, need);

    ASSERT_GE(m, 0) << "n=" << n;
    ASSERT_EQ(static_cast<size_t>(m), n) << "n=" << n;
    EXPECT_EQ(out[-1], 0xA5) << "underrun n=" << n;
    EXPECT_EQ(out[need], 0x5A) << "overrun n=" << n;
    EXPECT_EQ(out[m], '\0') << "terminator n=" << n;
    if (n) EXPECT_EQ(memcmp(out, in, n), 0) << "n=" << n;
  }
}

// ── Cross-module coherence ────────────────────────────────────────────────

TEST(Base64UrlInterop, LenientStdDecoderAcceptsUrlOutput)
{
  // The shared core must be internally consistent: base64_decode_lenient()
  // (standard alphabet, lenient) accepts '-'/'_', so anything base64url_encode()
  // produces must decode identically through it.
  const unsigned char in[] = "hello-world_123!";
  unsigned char enc[64] = {0};
  base64url_encode(in, sizeof(in) - 1, enc);

  int n = 0;
  unsigned char *dec = base64_decode_lenient(
      enc, static_cast<int>(strlen(reinterpret_cast<char *>(enc))), &n);
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(n, static_cast<int>(sizeof(in) - 1));
  EXPECT_EQ(memcmp(dec, in, static_cast<size_t>(n)), 0);
  free(dec);
}

}  // namespace
