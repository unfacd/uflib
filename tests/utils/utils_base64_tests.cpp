/**
 * @file utils_base64_tests.cpp
 * @brief Adversarial test suite for the utils_base64 module (RFC 4648 base64).
 *
 * The public API surface is five functions, declared in <uflib/utils_base64.h>
 * and implemented in src/utils_base64.c:
 *
 *   - size_t  GetBase64BufferAllocationSize(size_t str_sz)
 *   - ssize_t GetBase64BufferDecodedSize(const char *b64_encoded)
 *   - unsigned char *base64_encode(const unsigned char *buffer, int length, unsigned char *str_provided)
 *   - unsigned char *base64_decode(const unsigned char *str, int length, int *ret)
 *   - unsigned char *base64_decode_buffered(const unsigned char *str, int length, unsigned char *decoded_in, int *ret)
 *
 * This suite pairs ordinary contract tests (an independent reference encoder,
 * round-trip properties, known RFC 4648 vectors) with deliberately hostile
 * inputs designed to expose the module's hidden flaws rather than merely record
 * its current behaviour.  The hostile cases are:
 *
 *   - base64_encode(NULL, n>0, NULL) dereferences a NULL input buffer — asserted
 *     with a forked, alarm-bounded child so the fault cannot block the suite.
 *   - base64_encode(buf, -1/-2, NULL) slips past the overflow guard and writes
 *     through a malloc(0) allocation — asserted with a forked child.
 *   - base64_encode(buf, 0, NULL) writes a NUL into a malloc(0) allocation —
 *     asserted with a forked child (caught by ASan).
 *   - base64_decode() silently drops non-alphabet characters — asserted that a
 *     robust decoder rejects the input instead.
 *   - base64_decode_buffered() free()s a caller-provided buffer on the
 *     padding-error path — asserted with a forked child and a stack buffer.
 *   - GetBase64BufferDecodedSize() rejects every valid single-group (4-char)
 *     string because its minimum-input bound is an output-size formula — asserted
 *     against the correct decoded size.
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

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include "utilis_base64_test_vectors.h"

extern "C" {
#include "uflib/utils_base64.h"
}

namespace {

// RFC 4648 base64 alphabet.
static const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Independent reference encoder.  This is the oracle the module is judged
// against, so a disagreement is a finding, not an adjustment of the oracle.
std::string
EncodeRef(const unsigned char *bytes, size_t n)
{
  std::string out;
  size_t i = 0;
  while (i + 3 <= n) {
    uint32_t v = (static_cast<uint32_t>(bytes[i]) << 16) |
                 (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                 static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kB64Alphabet[(v >> 18) & 0x3Fu]);
    out.push_back(kB64Alphabet[(v >> 12) & 0x3Fu]);
    out.push_back(kB64Alphabet[(v >> 6) & 0x3Fu]);
    out.push_back(kB64Alphabet[v & 0x3Fu]);
    i += 3;
  }
  const size_t rem = n - i;
  if (rem == 1) {
    uint32_t v = static_cast<uint32_t>(bytes[i]) << 16;
    out.push_back(kB64Alphabet[(v >> 18) & 0x3Fu]);
    out.push_back(kB64Alphabet[(v >> 12) & 0x3Fu]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    uint32_t v = (static_cast<uint32_t>(bytes[i]) << 16) |
                 (static_cast<uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kB64Alphabet[(v >> 18) & 0x3Fu]);
    out.push_back(kB64Alphabet[(v >> 12) & 0x3Fu]);
    out.push_back(kB64Alphabet[(v >> 6) & 0x3Fu]);
    out.push_back('=');
  }
  return out;
}

// Run `fn` in a forked child that self-destructs via alarm() after
// `timeout_sec`.  Lets hostile inputs (faults, infinite loops, process exit)
// be probed without hanging or killing the test process itself.
struct ChildOutcome {
  bool exited;
  int exit_code;
  bool signaled;
  int sig;
};

ChildOutcome
RunInChild(const std::function<void()> &fn, unsigned timeout_sec = 1)
{
  pid_t pid = fork();
  if (pid < 0) {
    return ChildOutcome{false, -1, false, 0};
  }
  if (pid == 0) {
    alarm(timeout_sec);
    fn();
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  ChildOutcome o{false, 0, false, 0};
  if (WIFEXITED(status)) { o.exited = true; o.exit_code = WEXITSTATUS(status); }
  else if (WIFSIGNALED(status)) { o.signaled = true; o.sig = WTERMSIG(status); }
  return o;
}

// A deterministic byte pattern that is a function of the index only.
void
FillPattern(unsigned char *b, size_t n)
{
  for (size_t i = 0; i < n; ++i) {
    b[i] = static_cast<unsigned char>((i * 131u + 7u) & 0xFFu);
  }
}

// Number of decoded bytes for a base64 string: count the data characters
// (skipping whitespace, stopping at '=') and scale by 3/4.  Works for both
// canonical and lenient vectors.
size_t
ExpectedDecodedLen(const char *b64)
{
  size_t d = 0;
  for (const char *p = b64; *p != '\0'; ++p) {
    if (*p == '=') break;
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f') continue;
    ++d;
  }
  return (d * 3) / 4;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// base64_encode — correctness against an independent oracle
// ════════════════════════════════════════════════════════════════════════════

TEST(Base64Encode, MatchesReferenceAllLengths) {
  for (size_t n = 1; n <= 40; ++n) {
    unsigned char bytes[40];
    FillPattern(bytes, n);
    const std::string want = EncodeRef(bytes, n);

    unsigned char *got = base64_encode(bytes, static_cast<int>(n), nullptr);
    ASSERT_NE(got, nullptr) << "length " << n;
    EXPECT_EQ(std::string(reinterpret_cast<char *>(got)), want) << "length " << n;
    EXPECT_EQ(got[want.size()], '\0') << "length " << n;
    free(got);
  }
}

TEST(Base64Encode, KnownVectors) {
  struct { const char *in; int n; const char *want; } cases[] = {
    {"", 0, ""},
    {"f", 1, "Zg=="},
    {"fo", 2, "Zm8="},
    {"foo", 3, "Zm9v"},
    {"foob", 4, "Zm9vYg=="},
    {"fooba", 5, "Zm9vYmE="},
    {"foobar", 6, "Zm9vYmFy"},
  };
  for (const auto &c : cases) {
    unsigned char buf[16];
    unsigned char *got = base64_encode(
        reinterpret_cast<const unsigned char *>(c.in), c.n, buf);
    EXPECT_EQ(got, buf);
    EXPECT_STREQ(reinterpret_cast<char *>(got), c.want);
  }
}

TEST(Base64Encode, ExhaustiveAllByteValues) {
  unsigned char bytes[256];
  for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);

  const std::string want = EncodeRef(bytes, 256);
  unsigned char *got = base64_encode(bytes, 256, nullptr);

  ASSERT_NE(got, nullptr);
  EXPECT_EQ(std::string(reinterpret_cast<char *>(got)), want);
  EXPECT_EQ(got[want.size()], '\0');
  free(got);
}

TEST(Base64Encode, CallerProvidedBufferRoundTrip) {
  unsigned char bytes[16];
  FillPattern(bytes, sizeof(bytes));
  const std::string want = EncodeRef(bytes, sizeof(bytes));

  unsigned char buf[64] = {0};
  unsigned char *got = base64_encode(bytes, static_cast<int>(sizeof(bytes)), buf);

  EXPECT_EQ(got, buf);
  EXPECT_EQ(std::string(reinterpret_cast<char *>(buf)), want);
}

// ════════════════════════════════════════════════════════════════════════════
// base64_decode — correctness and round-trip
// ════════════════════════════════════════════════════════════════════════════

TEST(Base64Decode, KnownVectors) {
  struct { const char *b64; const char *want; int want_len; } cases[] = {
    {"Zg==", "f", 1},
    {"Zm8=", "fo", 2},
    {"Zm9v", "foo", 3},
    {"Zm9vYg==", "foob", 4},
    {"Zm9vYmE=", "fooba", 5},
    {"Zm9vYmFy", "foobar", 6},
  };
  for (const auto &c : cases) {
    int ret = -1;
    unsigned char *got = base64_decode(
        reinterpret_cast<const unsigned char *>(c.b64),
        static_cast<int>(strlen(c.b64)), &ret);
    ASSERT_NE(got, nullptr) << c.b64;
    EXPECT_EQ(ret, c.want_len) << c.b64;
    EXPECT_EQ(memcmp(got, c.want, static_cast<size_t>(c.want_len)), 0) << c.b64;
    free(got);
  }
}

TEST(Base64Decode, RoundTripAllLengths) {
  for (size_t n = 1; n <= 40; ++n) {
    unsigned char bytes[40];
    FillPattern(bytes, n);

    unsigned char enc[64];
    unsigned char *e = base64_encode(bytes, static_cast<int>(n), enc);
    ASSERT_NE(e, nullptr) << "length " << n;

    int ret = -1;
    unsigned char *d = base64_decode(
        e, static_cast<int>(strlen(reinterpret_cast<char *>(e))), &ret);
    ASSERT_NE(d, nullptr) << "length " << n;
    EXPECT_EQ(ret, static_cast<int>(n)) << "length " << n;
    EXPECT_EQ(memcmp(d, bytes, n), 0) << "length " << n;
    free(d);
  }
}

TEST(Base64Decode, RoundTripAllByteValues) {
  unsigned char bytes[256];
  for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);

  unsigned char *e = base64_encode(bytes, 256, nullptr);
  ASSERT_NE(e, nullptr);

  int ret = -1;
  unsigned char *d = base64_decode(
      e, static_cast<int>(strlen(reinterpret_cast<char *>(e))), &ret);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(ret, 256);
  EXPECT_EQ(memcmp(d, bytes, 256), 0);
  free(e);
  free(d);
}

TEST(Base64Decode, EmptyString) {
  int ret = -1;
  unsigned char *d = base64_decode(
      reinterpret_cast<const unsigned char *>(""), 0, &ret);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(d[0], '\0');
  free(d);
}

TEST(Base64Decode, MalformedPaddingReturnsNull) {
  // "Z=" places '=' where a second character is required (i % 4 == 1) — the
  // one path where the decoder signals an error by returning NULL.
  int ret = -1;
  unsigned char *d = base64_decode(
      reinterpret_cast<const unsigned char *>("Z="), 2, &ret);
  EXPECT_EQ(d, nullptr);
}

TEST(Base64Decode, StopsAtPadding) {
  // Bytes after the padding group are ignored.
  int ret = -1;
  unsigned char *d = base64_decode(
      reinterpret_cast<const unsigned char *>("Zm9vYg==junk"), 11, &ret);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(ret, 4);
  EXPECT_EQ(memcmp(d, "foob", 4), 0);
  free(d);
}

TEST(Base64Decode, StopsAtNullWhenLengthOverstated) {
  int ret = -1;
  unsigned char *d = base64_decode(
      reinterpret_cast<const unsigned char *>("Zg=="), 100, &ret);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(ret, 1);
  EXPECT_EQ(d[0], 'f');
  free(d);
}

// ════════════════════════════════════════════════════════════════════════════
// base64_decode_buffered — caller-buffer and calloc paths
// ════════════════════════════════════════════════════════════════════════════

TEST(Base64DecodeBuffered, CallerBufferDecodes) {
  unsigned char buf[16] = {0};
  int ret = -1;
  unsigned char *p = base64_decode_buffered(
      reinterpret_cast<const unsigned char *>("Zm9vYg=="), 8, buf, &ret);
  EXPECT_EQ(p, buf);
  EXPECT_EQ(ret, 4);
  EXPECT_EQ(memcmp(buf, "foob", 4), 0);
  EXPECT_EQ(buf[4], '\0');
}

TEST(Base64DecodeBuffered, CallocPathDecodes) {
  int ret = -1;
  unsigned char *p = base64_decode_buffered(
      reinterpret_cast<const unsigned char *>("Zm9vYg=="), 8, nullptr, &ret);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(ret, 4);
  EXPECT_EQ(memcmp(p, "foob", 4), 0);
  EXPECT_EQ(p[4], '\0');
  free(p);
}

TEST(Base64DecodeBuffered, PartialGroupDoesNotOverflow) {
  // A partial final group must produce only its real bytes plus the NUL
  // terminator.  The pre-fix case-1/case-2 handlers wrote a spurious trailing
  // byte and the padding handler wrote another zero byte, past an exactly-sized
  // caller buffer.
  struct Case {
    const char *b64;
    int decoded_len;
    const char *want;
  };
  const Case cases[] = {
    {"Zg==", 1, "f"},   // 1 remainder byte  -> "XX=="
    {"Zm8=", 2, "fo"},  // 2 remainder bytes -> "XXX="
  };
  for (const Case &c : cases) {
    struct {
      unsigned char buf[3];
      unsigned char guard[16];
    } s;
    memset(s.buf, 0xAA, sizeof(s.buf));
    memset(s.guard, 0x5A, sizeof(s.guard));

    int ret = -1;
    unsigned char *p = base64_decode_buffered(
        reinterpret_cast<const unsigned char *>(c.b64),
        static_cast<int>(strlen(c.b64)), s.buf, &ret);

    EXPECT_EQ(p, s.buf) << c.b64;
    EXPECT_EQ(ret, c.decoded_len) << c.b64;
    EXPECT_EQ(memcmp(s.buf, c.want, static_cast<size_t>(c.decoded_len)), 0) << c.b64;
    EXPECT_EQ(s.buf[c.decoded_len], '\0') << c.b64 << " (NUL terminator missing)";

    // Everything after [decoded_len] must be untouched — the decoder must write
    // exactly decoded_len bytes plus the NUL terminator.
    bool tail_intact = true;
    for (size_t idx = static_cast<size_t>(c.decoded_len) + 1; idx < sizeof(s.buf); ++idx) {
      if (s.buf[idx] != 0xAA) tail_intact = false;
    }
    bool guard_intact = true;
    for (unsigned char g : s.guard) {
      if (g != 0x5A) guard_intact = false;
    }
    EXPECT_TRUE(tail_intact) << c.b64 << " (wrote past decoded bytes + NUL)";
    EXPECT_TRUE(guard_intact) << c.b64 << " (wrote past the caller buffer)";
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Size helpers — contract against first-principles formulas
// ════════════════════════════════════════════════════════════════════════════

TEST(GetBase64BufferAllocationSize, SufficientUpperBound) {
  for (size_t n = 0; n <= 1000; ++n) {
    // Exact encoded length (4 chars per 3 bytes, incl. '=' padding) + NUL.
    const size_t exact = 4 * ((n + 2) / 3) + 1;
    EXPECT_GE(GetBase64BufferAllocationSize(n), exact) << "n=" << n;
  }
}

TEST(GetBase64BufferAllocationSize, EncodeFitsInProvidedSize) {
  for (size_t n = 0; n <= 300; ++n) {
    unsigned char bytes[300];
    FillPattern(bytes, n);

    struct {
      unsigned char buf[1024];
      unsigned char guard[16];
    } s;
    memset(s.guard, 0x5A, sizeof(s.guard));

    const size_t cap = GetBase64BufferAllocationSize(n);
    unsigned char *got = base64_encode(bytes, static_cast<int>(n), s.buf);
    ASSERT_NE(got, nullptr) << "n=" << n;

    const size_t outlen = strlen(reinterpret_cast<char *>(s.buf));
    EXPECT_LE(outlen + 1, cap) << "n=" << n;

    bool guard_intact = true;
    for (unsigned char c : s.guard) {
      if (c != 0x5A) guard_intact = false;
    }
    EXPECT_TRUE(guard_intact) << "n=" << n << " (wrote past caller buffer)";
  }
}

TEST(GetBase64BufferDecodedSize, PaddedVectors) {
  struct { const char *b64; ssize_t want; } cases[] = {
    {"Zm9vYg==", 4},
    {"Zm9vYmE=", 5},
    {"Zm9vYmFy", 6},
  };
  for (const auto &c : cases) {
    EXPECT_EQ(GetBase64BufferDecodedSize(c.b64), c.want) << c.b64;
  }
}

TEST(GetBase64BufferDecodedSize, SingleGroupStringsRejected) {
  // A valid 4-character (single-group) base64 string is reported as -1 because
  // the minimum-input bound is derived from an output-size formula
  // (GetBase64BufferAllocationSize(1) == 6) rather than the minimum valid
  // base64 length.  The correct decoded size is asserted.
  struct { const char *b64; ssize_t want; } cases[] = {
    {"Zg==", 1},
    {"Zm8=", 2},
    {"Zm9v", 3},
  };
  for (const auto &c : cases) {
    EXPECT_EQ(GetBase64BufferDecodedSize(c.b64), c.want) << c.b64;
  }
}

TEST(GetBase64BufferDecodedSize, EmptyAndShortRejected) {
  EXPECT_EQ(GetBase64BufferDecodedSize(""), static_cast<ssize_t>(-1));
  EXPECT_EQ(GetBase64BufferDecodedSize("Zg"), static_cast<ssize_t>(-1));
}

// ════════════════════════════════════════════════════════════════════════════
// Adversarial — memory-safety and robustness under hostile input
// ════════════════════════════════════════════════════════════════════════════

TEST(Base64EncodeAdversarial, NullBufferDoesNotCrash) {
  // Correct contract: NULL input with length > 0 returns NULL.  The current
  // implementation dereferences `buffer` unconditionally.
  const ChildOutcome o = RunInChild([] {
    base64_encode(nullptr, 3, nullptr);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "base64_encode(NULL, 3, NULL) faulted "
      << "(signaled=" << o.signaled << ", sig=" << o.sig
      << ", exit_code=" << o.exit_code << ")";
}

TEST(Base64EncodeAdversarial, NegativeLengthReturnsNull) {
  // length == -1 and -2 slip past the overflow guard ((length + 2) < 0 is
  // false for both) and then write through a malloc(0) allocation, encoding a
  // byte the caller never provided.  The correct contract is to return NULL.
  for (int len : {-1, -2}) {
    unsigned char byte = 0xAB;
    const ChildOutcome o = RunInChild([len, &byte] {
      unsigned char *p = base64_encode(&byte, len, nullptr);
      free(p);
      _exit(p == nullptr ? 0 : 1);
    });
    EXPECT_TRUE(o.exited && o.exit_code == 0)
        << "base64_encode(buf, " << len << ", NULL) faulted or returned non-NULL "
        << "(signaled=" << o.signaled << ", sig=" << o.sig
        << ", exit_code=" << o.exit_code << ")";
  }
}

TEST(Base64EncodeAdversarial, ZeroLengthMallocPathDoesNotCrash) {
  // Encoding zero bytes with no caller buffer malloc()s 0 bytes and then
  // writes a NUL terminator into it.  Writing into a malloc(0) allocation is UB
  // (C11 7.22.3.1); it does not fault under glibc, and ASan rounds zero-size
  // allocations to a usable chunk so it does not flag the write either.  The
  // test asserts the observable contract: a valid, freeable empty string.
  unsigned char byte = 0xAB;
  const ChildOutcome o = RunInChild([&byte] {
    unsigned char *p = base64_encode(&byte, 0, nullptr);
    if (p == nullptr) _exit(1);
    const bool empty = (p[0] == '\0');
    free(p);
    _exit(empty ? 0 : 2);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "base64_encode(buf, 0, NULL) faulted or returned wrong content "
      << "(signaled=" << o.signaled << ", sig=" << o.sig
      << ", exit_code=" << o.exit_code << ")";
}

TEST(Base64EncodeAdversarial, LargeBufferRoundTrips) {
  // A large (but feasible) input exercises the size_t allocation path without
  // the OOM risk of a near-INT_MAX length, and proves the size computation
  // does not overflow.
  const size_t N = 65536;
  std::vector<unsigned char> bytes(N);
  for (size_t i = 0; i < N; ++i) bytes[i] = static_cast<unsigned char>(i & 0xFFu);

  unsigned char *e = base64_encode(bytes.data(), static_cast<int>(N), nullptr);
  ASSERT_NE(e, nullptr);

  int ret = -1;
  unsigned char *d = base64_decode(
      e, static_cast<int>(strlen(reinterpret_cast<char *>(e))), &ret);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(ret, static_cast<int>(N));
  EXPECT_EQ(memcmp(d, bytes.data(), N), 0);
  free(e);
  free(d);
}

TEST(Base64DecodeAdversarial, NullStrDoesNotCrash) {
  const ChildOutcome o = RunInChild([] {
    int ret = 0;
    unsigned char *p = base64_decode(nullptr, 4, &ret);
    free(p);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "base64_decode(NULL, 4, &ret) faulted "
      << "(signaled=" << o.signaled << ", sig=" << o.sig
      << ", exit_code=" << o.exit_code << ")";
}

TEST(Base64DecodeAdversarial, NullRetDoesNotCrash) {
  const ChildOutcome o = RunInChild([] {
    unsigned char *p = base64_decode(
        reinterpret_cast<const unsigned char *>("Zg=="), 4, nullptr);
    free(p);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "base64_decode(\"Zg==\", 4, NULL) faulted "
      << "(signaled=" << o.signaled << ", sig=" << o.sig
      << ", exit_code=" << o.exit_code << ")";
}

TEST(Base64DecodeAdversarial, InvalidCharRejected) {
  // A robust decoder rejects non-alphabet input rather than silently dropping
  // it (which makes "Zg$==" and "Zg==" decode identically).  The current
  // implementation skips '$' and returns "f".
  int ret = -1;
  unsigned char *p = base64_decode(
      reinterpret_cast<const unsigned char *>("Zg$=="), 5, &ret);
  const bool is_null = (p == nullptr);
  free(p);
  EXPECT_TRUE(is_null) << "decoder silently accepted invalid input (decoded "
                       << ret << " bytes)";
}

TEST(Base64DecodeAdversarial, BufferedPaddingErrorDoesNotFreeCallerBuffer) {
  // "Z=" trips the i % 4 == 1 padding-error path, which calls free(result).
  // When the caller supplied decoded_in, result aliases the caller's buffer,
  // so free() is invoked on a caller-owned (here: stack) pointer.
  const ChildOutcome o = RunInChild([] {
    unsigned char buf[16] = {0};
    int ret = -1;
    unsigned char *p = base64_decode_buffered(
        reinterpret_cast<const unsigned char *>("Z="), 2, buf, &ret);
    _exit(p == nullptr ? 0 : 1);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "base64_decode_buffered() freed a caller-provided buffer "
      << "(signaled=" << o.signaled << ", sig=" << o.sig
      << ", exit_code=" << o.exit_code << ")";
}

TEST(Base64DecodeAdversarial, BufferedInvalidCharDoesNotFreeCallerBuffer) {
  // The same free()-on-caller-buffer hazard as the padding case, but triggered
  // through the invalid-character rejection path instead of '='.
  const ChildOutcome o = RunInChild([] {
    unsigned char buf[16] = {0};
    int ret = -1;
    unsigned char *p = base64_decode_buffered(
        reinterpret_cast<const unsigned char *>("Zg$=="), 5, buf, &ret);
    _exit(p == nullptr ? 0 : 1);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "base64_decode_buffered() with invalid char faulted "
      << "(signaled=" << o.signaled << ", sig=" << o.sig
      << ", exit_code=" << o.exit_code << ")";
}

// ════════════════════════════════════════════════════════════════════════════
// base64_decode — malformed padding must be rejected, not silently truncated
// ════════════════════════════════════════════════════════════════════════════

TEST(Base64DecodeMalformedPadding, AllPaddingRejected) {
  // "====" carries no data before the padding — it must not decode to "".
  int ret = -1;
  unsigned char *p = base64_decode(
      reinterpret_cast<const unsigned char *>("===="), 4, &ret);
  const bool is_null = (p == nullptr);
  free(p);
  EXPECT_TRUE(is_null) << "decoder accepted a padding-only input";
}

TEST(Base64DecodeMalformedPadding, LeadingPaddingRejected) {
  // A '=' before any data character is not base64.
  for (const char *bad : {"=Zg==", "=abc"}) {
    int ret = -1;
    unsigned char *p = base64_decode(
        reinterpret_cast<const unsigned char *>(bad),
        static_cast<int>(strlen(bad)), &ret);
    const bool is_null = (p == nullptr);
    free(p);
    EXPECT_TRUE(is_null) << bad;
  }
}

TEST(Base64DecodeMalformedPadding, PaddingAfterFullGroupRejected) {
  // "Zm9v" is already a complete group (3 bytes); appending "==" is malformed.
  int ret = -1;
  unsigned char *p = base64_decode(
      reinterpret_cast<const unsigned char *>("Zm9v=="), 6, &ret);
  const bool is_null = (p == nullptr);
  free(p);
  EXPECT_TRUE(is_null) << "decoder accepted padding after a complete group";
}

TEST(Base64DecodeMalformedPadding, WrongPadCountRejected) {
  // For a 2-character group the only canonical padding is "==".
  for (const char *bad : {"Zg=", "Zg==="}) {
    int ret = -1;
    unsigned char *p = base64_decode(
        reinterpret_cast<const unsigned char *>(bad),
        static_cast<int>(strlen(bad)), &ret);
    const bool is_null = (p == nullptr);
    free(p);
    EXPECT_TRUE(is_null) << bad;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Strict test vectors (utilis_base64_test_vectors.h)
// ════════════════════════════════════════════════════════════════════════════

TEST(Base64DecodeStrictVectors, StrictTestVectors) {
  for (const auto &v : strict_tests) {
    int ret = -1;
    unsigned char *p = base64_decode(
        reinterpret_cast<const unsigned char *>(v.encoded),
        static_cast<int>(strlen(v.encoded)), &ret);

    if (v.should_pass) {
      ASSERT_NE(p, nullptr) << v.description;
      const size_t want_len = ExpectedDecodedLen(v.encoded);
      EXPECT_EQ(ret, static_cast<int>(want_len)) << v.description;
      EXPECT_EQ(memcmp(p, v.expected_hex, want_len), 0) << v.description;
    } else {
      EXPECT_EQ(p, nullptr) << v.description << " (should have been rejected)";
    }
    free(p);
  }
}

TEST(Base64DecodeLenientVectors, LenientTestVectors) {
  for (const auto &v : lenient_tests) {
    int ret = -1;
    unsigned char *p = base64_decode_lenient(
        reinterpret_cast<const unsigned char *>(v.encoded),
        static_cast<int>(strlen(v.encoded)), &ret);

    if (v.should_pass) {
      ASSERT_NE(p, nullptr) << v.description;
      const size_t want_len = ExpectedDecodedLen(v.encoded);
      EXPECT_EQ(ret, static_cast<int>(want_len)) << v.description;
      EXPECT_EQ(memcmp(p, v.expected_hex, want_len), 0) << v.description;
    } else {
      EXPECT_EQ(p, nullptr) << v.description << " (should have been rejected)";
    }
    free(p);
  }
}

TEST(GetBase64BufferDecodedSizeAdversarial, NullInputDoesNotCrash) {
  const ChildOutcome o = RunInChild([] {
    (void)GetBase64BufferDecodedSize(nullptr);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "GetBase64BufferDecodedSize(NULL) faulted "
      << "(signaled=" << o.signaled << ", sig=" << o.sig
      << ", exit_code=" << o.exit_code << ")";
}
