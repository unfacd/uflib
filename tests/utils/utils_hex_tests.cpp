/**
 * @file utils_hex_tests.cpp
 * @brief Adversarial test suite for the utils_hex module (bin2hex / hexchr2bin / hex2bin).
 *
 * The utils_hex API surface is three functions, all declared in
 * <uflib/utils_hex.h> and implemented in src/utils_hex.c:
 *
 *   - char   *bin2hex(const unsigned char *bin, size_t len, char *result_out)
 *   - int      hexchr2bin(const char hex, char *out)
 *   - size_t   hex2bin(const char *hex, unsigned char **out)
 *
 * This suite pairs ordinary contract tests with deliberately hostile inputs —
 * NULL pointers, the empty string, odd-length strings, exhaustive byte/char
 * sweeps, round-trip asymmetries, and inputs designed to expose the module's
 * hidden flaws (a leak-on-invalid-decode and an unchecked malloc) — rather than
 * merely recording its current behaviour.
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

#include "gtest/gtest.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

// <atomic> is pre-loaded with C++ linkage *before* the extern "C" block:
// utils_hex.h transitively pulls in <uflib/standard_c_includes.h>, whose C++
// branch #includes <atomic>; loading it first lets that include become a no-op
// (include guard) rather than instantiating templates under C linkage.
extern "C" {
#include "uflib/utils_hex.h"
}

namespace {

// bin2hex always emits the uppercase table "0123456789ABCDEF".  This reference
// encoder lets the suite assert exact output rather than re-deriving it inline.
std::string
EncodeRef(const unsigned char *bytes, size_t n)
{
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(kHex[bytes[i] >> 4]);
    out.push_back(kHex[bytes[i] & 0x0Fu]);
  }
  return out;
}

// Lowercase reference encoder — bin2hex_case(..., true, ...) output.
std::string
EncodeRefLower(const unsigned char *bytes, size_t n)
{
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(kHex[bytes[i] >> 4]);
    out.push_back(kHex[bytes[i] & 0x0Fu]);
  }
  return out;
}

// A sentinel that can never be a valid nibble (0-15) so an "untouched" output
// is distinguishable from any value hexchr2bin might legitimately write.
constexpr char kSentinel = static_cast<char>(0x7F);

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// bin2hex — contract and boundary
// ════════════════════════════════════════════════════════════════════════════

TEST(Bin2hex, NullInputReturnsNull) {
  EXPECT_EQ(bin2hex(nullptr, 1, nullptr), nullptr);
  EXPECT_EQ(bin2hex(nullptr, 0, nullptr), nullptr);
}

TEST(Bin2hex, ZeroLengthReturnsNullWithoutWriting) {
  // len == 0 short-circuits to NULL before result_out is touched, so a
  // caller-supplied buffer must be left exactly as it was.
  char buf[4] = {'?', '?', '?', '?'};
  const unsigned char empty = 0;
  EXPECT_EQ(bin2hex(&empty, 0, buf), nullptr);
  EXPECT_EQ(buf[0], '?');
  EXPECT_EQ(buf[3], '?');
}

TEST(Bin2hex, SingleBytes) {
  struct { unsigned char in; const char *want; } cases[] = {
    {0x00, "00"}, {0x01, "01"}, {0x09, "09"}, {0x0A, "0A"},
    {0x0F, "0F"}, {0x10, "10"}, {0xA5, "A5"}, {0xFE, "FE"}, {0xFF, "FF"},
  };
  for (const auto &c : cases) {
    char *hex = bin2hex(&c.in, 1, nullptr);
    ASSERT_NE(hex, nullptr);
    EXPECT_STREQ(hex, c.want);
    free(hex);
  }
}

TEST(Bin2hex, IsUppercaseNotLowercase) {
  const unsigned char in[] = {0xDE, 0xAD, 0xBE, 0xEF};
  char *hex = bin2hex(in, sizeof(in), nullptr);
  ASSERT_NE(hex, nullptr);
  EXPECT_STREQ(hex, "DEADBEEF");
  EXPECT_STRNE(hex, "deadbeef");
  free(hex);
}

TEST(Bin2hex, TerminatesWithNul) {
  const unsigned char in[] = {0xAB, 0xCD};
  char *hex = bin2hex(in, 2, nullptr);
  ASSERT_NE(hex, nullptr);
  EXPECT_EQ(hex[4], '\0');
  free(hex);
}

TEST(Bin2hex, ExhaustiveAllByteValues) {
  unsigned char bytes[256];
  for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);

  char *hex = bin2hex(bytes, 256, nullptr);
  ASSERT_NE(hex, nullptr);
  EXPECT_EQ(std::string(hex), EncodeRef(bytes, 256));
  free(hex);
}

TEST(Bin2hex, CallerBufferReturnsSamePointer) {
  const unsigned char in[] = {0x11, 0x22, 0x33};
  char buf[7];
  char *r = bin2hex(in, 3, buf);
  EXPECT_EQ(r, buf);
  EXPECT_STREQ(buf, "112233");
}

TEST(Bin2hex, CallerBufferExactSizedNoOverrun) {
  // result_out sized to exactly 2*len+1 must hold the text and its NUL; the
  // byte after the NUL must stay untouched (guard byte).
  const unsigned char in[] = {0xCA, 0xFE};
  struct { char text[5]; char guard; } buf = {{0}, 'G'};
  char *r = bin2hex(in, 2, buf.text);
  EXPECT_EQ(r, buf.text);
  EXPECT_STREQ(buf.text, "CAFE");
  EXPECT_EQ(buf.guard, 'G');
}

// ════════════════════════════════════════════════════════════════════════════
// bin2hex_case — case selection
// ════════════════════════════════════════════════════════════════════════════

TEST(Bin2hexCase, LowercaseSingleBytes) {
  struct { unsigned char in; const char *want; } cases[] = {
    {0x00, "00"}, {0x0A, "0a"}, {0xA5, "a5"}, {0xDE, "de"}, {0xFF, "ff"},
  };
  for (const auto &c : cases) {
    char *hex = bin2hex_case(&c.in, 1, true, nullptr);
    ASSERT_NE(hex, nullptr);
    EXPECT_STREQ(hex, c.want);
    free(hex);
  }
}

TEST(Bin2hexCase, UppercaseMatchesDefaultBin2hex) {
  // is_small_letter_hex == false must reproduce bin2hex() exactly.
  const unsigned char in[] = {0xDE, 0xAD, 0xBE, 0xEF};
  char *upper = bin2hex_case(in, sizeof(in), false, nullptr);
  char *def   = bin2hex(in, sizeof(in), nullptr);
  ASSERT_NE(upper, nullptr);
  ASSERT_NE(def, nullptr);
  EXPECT_STREQ(upper, "DEADBEEF");
  EXPECT_STREQ(upper, def);
  free(upper);
  free(def);
}

TEST(Bin2hexCase, DefaultEncodingConstantIsUppercase) {
  // Lock the backward-compatible default: bin2hex() resolves to uppercase.
  EXPECT_FALSE(BIN2HEX_DEFAULT_LETTER_ENCODING);
  EXPECT_TRUE(BIN2HEX_SMALL_LETTER_ENCODING);
}

TEST(Bin2hexCase, ExhaustiveLowercaseAllByteValues) {
  unsigned char bytes[256];
  for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);
  char *hex = bin2hex_case(bytes, 256, true, nullptr);
  ASSERT_NE(hex, nullptr);
  EXPECT_EQ(std::string(hex), EncodeRefLower(bytes, 256));
  free(hex);
}

TEST(Bin2hexCase, ExhaustiveUppercaseAllByteValues) {
  unsigned char bytes[256];
  for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);
  char *hex = bin2hex_case(bytes, 256, false, nullptr);
  ASSERT_NE(hex, nullptr);
  EXPECT_EQ(std::string(hex), EncodeRef(bytes, 256));
  free(hex);
}

TEST(Bin2hexCase, NullAndEmptyReturnNull) {
  EXPECT_EQ(bin2hex_case(nullptr, 1, true, nullptr), nullptr);
  const unsigned char b = 0;
  EXPECT_EQ(bin2hex_case(&b, 0, true, nullptr), nullptr);
}

TEST(Bin2hexCase, CallerBufferReturnsSamePointer) {
  const unsigned char in[] = {0xAB};
  char buf[3];
  char *r = bin2hex_case(in, 1, true, buf);
  EXPECT_EQ(r, buf);
  EXPECT_STREQ(buf, "ab");
}

TEST(Bin2hexCase, RoundTripLowercaseThroughHex2bin) {
  // hex2bin is case-insensitive, so a lowercase encode must decode identically.
  unsigned char bytes[256];
  for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);
  char *hex = bin2hex_case(bytes, 256, true, nullptr);
  ASSERT_NE(hex, nullptr);
  unsigned char *decoded = nullptr;
  size_t n = hex2bin(hex, &decoded);
  ASSERT_EQ(n, 256u);
  EXPECT_EQ(memcmp(decoded, bytes, 256), 0);
  free(hex);
  free(decoded);
}

// ════════════════════════════════════════════════════════════════════════════
// hexchr2bin — exhaustive single-character decode
// ════════════════════════════════════════════════════════════════════════════

TEST(Hexchr2bin, ExhaustiveAllCharValues) {
  for (int c = 0; c <= 255; ++c) {
    char out = kSentinel;
    int rc = hexchr2bin(static_cast<char>(c), &out);

    int expected = -1;
    if (c >= '0' && c <= '9')       expected = c - '0';
    else if (c >= 'A' && c <= 'F')  expected = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f')  expected = c - 'a' + 10;

    if (expected >= 0) {
      EXPECT_EQ(rc, 1) << "char code " << c;
      EXPECT_EQ(static_cast<unsigned char>(out),
                static_cast<unsigned char>(expected)) << "char code " << c;
    } else {
      EXPECT_EQ(rc, 0) << "char code " << c;
      EXPECT_EQ(out, kSentinel) << "char code " << c;  // untouched on failure
    }
  }
}

TEST(Hexchr2bin, NullOutputReturnsZero) {
  // out == NULL is rejected before any dereference; valid input must still fail.
  EXPECT_EQ(hexchr2bin('A', nullptr), 0);
  EXPECT_EQ(hexchr2bin('0', nullptr), 0);
  EXPECT_EQ(hexchr2bin('z', nullptr), 0);
}

TEST(Hexchr2bin, BoundaryCharsAroundValidRanges) {
  // Just outside each valid range, in ASCII order.
  struct { char in; int want; } cases[] = {
    {'/', 0},   // just before '0'
    {':', 0},   // just after '9'
    {'@', 0},   // just before 'A'
    {'G', 0},   // just after 'F'
    {'`', 0},   // just before 'a'
    {'g', 0},   // just after 'f'
  };
  for (const auto &c : cases) {
    char out = kSentinel;
    EXPECT_EQ(hexchr2bin(c.in, &out), c.want) << "char '" << c.in << "'";
    EXPECT_EQ(out, kSentinel) << "char '" << c.in << "'";
  }
}

TEST(Hexchr2bin, CaseInsensitive) {
  for (int i = 0; i < 6; ++i) {
    char upper = static_cast<char>('A' + i);
    char lower = static_cast<char>('a' + i);
    char a = 0, b = 0;
    ASSERT_EQ(hexchr2bin(upper, &a), 1);
    ASSERT_EQ(hexchr2bin(lower, &b), 1);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, 10 + i);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// hex2bin — contract (NULL / empty / odd / NULL-out)
// ════════════════════════════════════════════════════════════════════════════

TEST(Hex2bin, NullInputReturnsZero) {
  unsigned char *out = nullptr;
  EXPECT_EQ(hex2bin(nullptr, &out), 0u);
  EXPECT_EQ(out, nullptr);
}

TEST(Hex2bin, EmptyInputReturnsZero) {
  unsigned char *out = nullptr;
  EXPECT_EQ(hex2bin("", &out), 0u);
  EXPECT_EQ(out, nullptr);
}

TEST(Hex2bin, NullOutReturnsZero) {
  EXPECT_EQ(hex2bin("00", nullptr), 0u);
  EXPECT_EQ(hex2bin("DEADBEEF", nullptr), 0u);
}

TEST(Hex2bin, OddLengthReturnsZeroWithoutAllocating) {
  unsigned char *out = nullptr;
  EXPECT_EQ(hex2bin("A", &out), 0u);
  EXPECT_EQ(out, nullptr);          // returns before malloc — passes
  EXPECT_EQ(hex2bin("ABC", &out), 0u);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(hex2bin("0", &out), 0u);
  EXPECT_EQ(out, nullptr);
}

// ════════════════════════════════════════════════════════════════════════════
// hex2bin — successful decode (case handling, round-trip)
// ════════════════════════════════════════════════════════════════════════════

TEST(Hex2bin, SingleByteDecode) {
  struct { const char *in; unsigned char want; } cases[] = {
    {"00", 0x00}, {"FF", 0xFF}, {"0F", 0x0F}, {"F0", 0xF0},
    {"a5", 0xA5}, {"A5", 0xA5}, {"ab", 0xAB}, {"7f", 0x7F},
  };
  for (const auto &c : cases) {
    unsigned char *out = nullptr;
    size_t n = hex2bin(c.in, &out);
    ASSERT_EQ(n, 1u) << c.in;
    ASSERT_NE(out, nullptr) << c.in;
    EXPECT_EQ(out[0], c.want) << c.in;
    free(out);
  }
}

TEST(Hex2bin, MultiByteDecodeUpperAndLower) {
  unsigned char *out = nullptr;
  size_t n = hex2bin("DEADBEEF", &out);
  ASSERT_EQ(n, 4u);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(memcmp(out, "\xDE\xAD\xBE\xEF", 4), 0);
  free(out);

  out = nullptr;
  n = hex2bin("deadbeef", &out);
  ASSERT_EQ(n, 4u);
  EXPECT_EQ(memcmp(out, "\xDE\xAD\xBE\xEF", 4), 0);
  free(out);
}

TEST(Hex2bin, MixedCaseDecode) {
  unsigned char *out = nullptr;
  size_t n = hex2bin("aBcD", &out);
  ASSERT_EQ(n, 2u);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out[0], 0xABu);
  EXPECT_EQ(out[1], 0xCDu);
  free(out);
}

TEST(Hex2bin, OutputIsRawBinaryNotNulTerminated) {
  // A 0x00 byte mid-stream must not truncate the decode; length is the truth.
  unsigned char *out = nullptr;
  size_t n = hex2bin("000142", &out);
  ASSERT_EQ(n, 3u);
  EXPECT_EQ(out[0], 0x00u);
  EXPECT_EQ(out[1], 0x01u);
  EXPECT_EQ(out[2], 0x42u);
  free(out);
}

TEST(Hex2bin, RoundTripAllByteValues) {
  unsigned char bytes[256];
  for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);

  char *hex = bin2hex(bytes, 256, nullptr);
  ASSERT_NE(hex, nullptr);

  unsigned char *decoded = nullptr;
  size_t n = hex2bin(hex, &decoded);
  ASSERT_EQ(n, 256u);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(memcmp(decoded, bytes, 256), 0);

  free(hex);
  free(decoded);
}

TEST(Hex2bin, RoundTripLargeBuffer) {
  constexpr size_t kN = 4096;
  unsigned char bytes[kN];
  for (size_t i = 0; i < kN; ++i) bytes[i] = static_cast<unsigned char>((i * 131u) & 0xFF);

  char *hex = bin2hex(bytes, kN, nullptr);
  ASSERT_NE(hex, nullptr);

  unsigned char *decoded = nullptr;
  size_t n = hex2bin(hex, &decoded);
  ASSERT_EQ(n, kN);
  EXPECT_EQ(memcmp(decoded, bytes, kN), 0);

  free(hex);
  free(decoded);
}

TEST(Hex2bin, EmbeddedNulTruncatesInput) {
  // strlen-based parsing stops at the first NUL, so "00\0FF" decodes only "00".
  const char raw[] = {'0', '0', '\0', 'F', 'F'};
  unsigned char *out = nullptr;
  size_t n = hex2bin(raw, &out);
  ASSERT_EQ(n, 1u);
  EXPECT_EQ(out[0], 0x00u);
  free(out);
}

// ════════════════════════════════════════════════════════════════════════════
// hex2bin — adversarial: invalid input must not allocate
// ════════════════════════════════════════════════════════════════════════════
//
// The contract for a failing decode is "nothing was allocated": the caller
// receives 0, owns nothing to free, and *out is left exactly as it was passed
// in.  These cases deliberately feed invalid (but even-length) input — leading,
// trailing, mid-string, and whitespace characters — and assert the buffer is
// untouched.  They guard against a regression to the historical behaviour where
// the allocation happened before validation and leaked on failure.

TEST(Hex2binAdversarial, InvalidLeadingCharDoesNotAllocate) {
  unsigned char *out = nullptr;
  EXPECT_EQ(hex2bin("G0", &out), 0u);
  EXPECT_EQ(out, nullptr);   // contract: a failed decode allocates nothing
}

TEST(Hex2binAdversarial, InvalidTrailingCharDoesNotAllocate) {
  unsigned char *out = nullptr;
  EXPECT_EQ(hex2bin("0G", &out), 0u);
  EXPECT_EQ(out, nullptr);
}

TEST(Hex2binAdversarial, InvalidMiddleCharDoesNotAllocate) {
  unsigned char *out = nullptr;
  EXPECT_EQ(hex2bin("DEADGEEF", &out), 0u);
  EXPECT_EQ(out, nullptr);
}

TEST(Hex2binAdversarial, WhitespaceIsInvalidAndDoesNotAllocate) {
  unsigned char *out = nullptr;
  EXPECT_EQ(hex2bin("0 ", &out), 0u);
  EXPECT_EQ(out, nullptr);

  out = nullptr;
  EXPECT_EQ(hex2bin("\t0", &out), 0u);
  EXPECT_EQ(out, nullptr);
}
