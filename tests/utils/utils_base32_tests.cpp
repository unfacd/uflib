/**
 * @file utils_base32_tests.cpp
 * @brief Adversarial test suite for the utils_base32 module (Crockford Base32).
 *
 * The public API surface is five functions, all declared in <uflib/base32.h>
 * and implemented in src/utils_base32.c:
 *
 *   - size_t base32enc(char *dest, const void *src, size_t s_len)
 *   - size_t base32dec(void *dest, size_t dest_len, const char *src)
 *   - size_t base32encsize(size_t count)
 *   - size_t base32decsize(size_t count)
 *   - size_t Base32ProvideEncodedBufferSize(size_t src_sz)
 *
 * This suite pairs ordinary contract tests (an independent reference encoder,
 * round-trip properties, known Crockford vectors) with deliberately hostile
 * inputs designed to expose the module's hidden flaws rather than merely record
 * its current behaviour.  The hostile cases are:
 *
 *   - base32enc(dest, src, 0) loops forever reading out of bounds — asserted
 *     with a forked, alarm-bounded child so the hang cannot block the suite.
 *   - base32dec() writes past `dest` when the buffer is smaller than the
 *     decoded payload — asserted with a stack guard canary.
 *   - base32dec() terminates the whole process via die() on an invalid
 *     character — asserted with a forked child that should return normally.
 *   - base32dec()'s small-buffer path returns 0 and copies garbage — asserted
 *     for the correct byte count and untouched trailing bytes.
 *   - base32decsize() returns 0/1 instead of floor(count*5/8) — asserted
 *     against the correct formula.
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
#include <functional>

#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

extern "C" {
#include "uflib/base32.h"
}

namespace {

// Crockford Base32 alphabet — excludes I, L, O, U.
static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Independent reference encoder: bit-packs 5 bytes into 8 chars, left-aligning
// a partial tail.  This is the oracle the module is judged against, so a
// disagreement is a finding, not an adjustment of the oracle.
std::string
EncodeRef(const unsigned char *bytes, size_t n)
{
  std::string out;
  size_t i = 0;
  while (i + 5 <= n) {
    uint64_t v = 0;
    for (int j = 0; j < 5; ++j) v = (v << 8) | bytes[i + j];
    // Extract the 40 bits MSB-first: 8 groups of 5 bits, most significant first.
    for (int j = 7; j >= 0; --j) {
      out.push_back(kAlphabet[(v >> (j * 5)) & 0x1Fu]);
    }
    i += 5;
  }
  const size_t rem = n - i;
  if (rem > 0) {
    uint64_t v = 0;
    for (size_t j = 0; j < rem; ++j) v = (v << 8) | bytes[i + j];
    const int nchars = static_cast<int>((rem * 8 + 4) / 5);
    v <<= (nchars * 5 - rem * 8);
    for (int j = 0; j < nchars; ++j) {
      const int shift = (nchars - 1 - j) * 5;
      out.push_back(kAlphabet[(v >> shift) & 0x1Fu]);
    }
  }
  return out;
}

// Run `fn` in a forked child that self-destructs via alarm() after
// `timeout_sec`.  Lets hostile inputs (infinite loops, process exit) be probed
// without hanging or killing the test process itself.
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

// A deterministic byte pattern that is a function of the index only, so every
// length produces a distinct, reproducible buffer.
void
FillPattern(unsigned char *b, size_t n)
{
  for (size_t i = 0; i < n; ++i) b[i] = static_cast<unsigned char>((i * 131u + 7u) & 0xFFu);
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// base32enc — encode correctness against an independent oracle
// ════════════════════════════════════════════════════════════════════════════

TEST(Base32enc, MatchesReferenceForAllLengths) {
  for (size_t n = 1; n <= 40; ++n) {
    unsigned char bytes[40];
    FillPattern(bytes, n);
    const std::string want = EncodeRef(bytes, n);

    char out[128];
    const size_t got = base32enc(out, bytes, n);

    EXPECT_EQ(got, want.size()) << "length " << n;
    EXPECT_EQ(std::string(out, got), want) << "length " << n;
    EXPECT_EQ(out[got], '\0') << "length " << n;
  }
}

TEST(Base32enc, KnownVectorFoobar) {
  // dest is sized to the padded block footprint (Base32ProvideEncodedBufferSize(6)+1
  // == 17), not the stripped length, because base32enc writes a full padded block
  // internally before trimming.
  char out[32];
  const size_t n = base32enc(out, "foobar", 6);
  EXPECT_EQ(n, 10u);
  EXPECT_STREQ(out, "CSQPYRK1E8");
}

TEST(Base32enc, KnownVectorSingleByteFF) {
  const unsigned char b = 0xFF;
  char out[32];
  const size_t n = base32enc(out, &b, 1);
  EXPECT_EQ(n, 2u);
  EXPECT_STREQ(out, "ZW");
}

TEST(Base32enc, KnownVectorSingleByte00) {
  const unsigned char b = 0x00;
  char out[32];
  const size_t n = base32enc(out, &b, 1);
  EXPECT_EQ(n, 2u);
  EXPECT_STREQ(out, "00");
}

TEST(Base32enc, ExhaustiveAllByteValues) {
  unsigned char bytes[256];
  for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);

  const std::string want = EncodeRef(bytes, 256);
  char out[512];
  const size_t n = base32enc(out, bytes, 256);

  EXPECT_EQ(n, want.size());
  EXPECT_EQ(std::string(out, n), want);
  EXPECT_EQ(out[n], '\0');
}

TEST(Base32enc, EmptyInputDoesNotHang) {
  // The correct contract for a zero-length encode is to return immediately
  // with length 0.  The current implementation instead underflows its loop
  // bound and reads out of bounds forever, so this child is killed by SIGALRM
  // (or a fault) rather than returning — exposing the defect without hanging
  // the suite.
  const ChildOutcome o = RunInChild([] {
    char dest[16] = {0};
    const unsigned char src[1] = {0};
    base32enc(dest, src, 0);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "base32enc(dest, src, 0) did not return normally "
      << "(signaled=" << o.signaled << ", sig=" << o.sig
      << ", exit_code=" << o.exit_code << ")";
}

// ════════════════════════════════════════════════════════════════════════════
// base32dec — decode correctness and round-trip
// ════════════════════════════════════════════════════════════════════════════

TEST(Base32dec, KnownVectorFoobar) {
  unsigned char out[16] = {0};
  const size_t n = base32dec(out, sizeof(out), "CSQPYRK1E8", true);
  EXPECT_EQ(n, 6u);
  EXPECT_EQ(memcmp(out, "foobar", 6), 0);
}

TEST(Base32dec, DecodeWithoutPadding) {
  // "ZW" is the unpadded form of 0xFF; the decoder must stop at the NUL.
  unsigned char out[16] = {0};
  const size_t n = base32dec(out, sizeof(out), "ZW", true);
  EXPECT_EQ(n, 1u);
  EXPECT_EQ(out[0], 0xFFu);
}

TEST(Base32dec, EmptyString) {
  unsigned char out[16] = {0};
  const size_t n = base32dec(out, sizeof(out), "", true);
  EXPECT_EQ(n, 0u);
}

TEST(Base32dec, RoundTripAllLengths) {
  for (size_t n = 1; n <= 40; ++n) {
    unsigned char bytes[40];
    FillPattern(bytes, n);

    char enc[128];
    const size_t enc_len = base32enc(enc, bytes, n);
    ASSERT_EQ(enc_len, EncodeRef(bytes, n).size()) << "length " << n;

    // Buffer generously: decode_block() always writes a full 5-byte group even
    // for a partial tail, so a buffer exactly `n` bytes can be overrun.
    unsigned char decoded[64] = {0};
    const size_t dec_len = base32dec(decoded, sizeof(decoded), enc, true);

    EXPECT_EQ(dec_len, n) << "length " << n;
    EXPECT_EQ(memcmp(decoded, bytes, n), 0) << "length " << n;
  }
}

TEST(Base32dec, RoundTripAllByteValues) {
  unsigned char bytes[256];
  for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);

  char enc[512];
  const size_t enc_len = base32enc(enc, bytes, 256);
  ASSERT_EQ(enc_len, EncodeRef(bytes, 256).size());

  unsigned char decoded[512] = {0};
  const size_t dec_len = base32dec(decoded, sizeof(decoded), enc, true);

  EXPECT_EQ(dec_len, 256u);
  EXPECT_EQ(memcmp(decoded, bytes, 256), 0);
}

TEST(Base32dec, BufferOverflowTruncates) {
  // The documented contract is that `dest_len` bounds the write ("data gets
  // truncated").  Decoding 10 bytes into a 6-byte buffer must not touch the
  // guard canary that follows it.
  const unsigned char data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  char enc[32];
  const size_t enc_len = base32enc(enc, data, sizeof(data));
  ASSERT_GT(enc_len, 0u);

  struct {
    unsigned char buf[6];
    unsigned char guard[16];
  } s;
  memset(s.buf, 0xAA, sizeof(s.buf));
  memset(s.guard, 0x5A, sizeof(s.guard));

  base32dec(s.buf, sizeof(s.buf), enc, true);

  bool guard_intact = true;
  for (unsigned char c : s.guard) {
    if (c != 0x5A) guard_intact = false;
  }
  EXPECT_TRUE(guard_intact)
      << "base32dec() wrote past a 6-byte dest buffer (truncation contract violated)";
}

TEST(Base32dec, SmallBufferReturnsByteCount) {
  // Decoding one byte ("ZW") into a 5-byte buffer must report 1 and leave the
  // trailing bytes untouched.  The small-buffer path instead returns 0 and
  // copies uninitialised bytes.
  struct {
    unsigned char buf[5];
  } s;
  memset(s.buf, 0xCC, sizeof(s.buf));

  const size_t n = base32dec(s.buf, sizeof(s.buf), "ZW", true);

  EXPECT_EQ(n, 1u);
  EXPECT_EQ(s.buf[0], 0xFFu);
  // Bytes 1..4 must be untouched by the call.
  for (size_t i = 1; i < sizeof(s.buf); ++i) {
    EXPECT_EQ(s.buf[i], 0xCCu) << "byte " << i;
  }
}

TEST(Base32dec, InvalidInputDoesNotExitProcess) {
  // 'I' is deliberately excluded from the Crockford alphabet.  A robust decoder
  // must signal an error without terminating the caller's process; the current
  // implementation calls die() -> exit(128).
  const ChildOutcome o = RunInChild([] {
    unsigned char dest[16] = {0};
    base32dec(dest, sizeof(dest), "INVALID!", true);
  });
  EXPECT_TRUE(o.exited && o.exit_code == 0)
      << "base32dec() with invalid input terminated the process "
      << "(exit_code=" << o.exit_code << ", signaled=" << o.signaled
      << ", sig=" << o.sig << ")";
}

// ════════════════════════════════════════════════════════════════════════════
// base32dec — leniency switch (strict vs tolerant decode)
// ════════════════════════════════════════════════════════════════════════════

TEST(Base32decLeniency, LowercaseAcceptedWhenLenient) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec(out, sizeof(out), "zw", true), 1u);
  EXPECT_EQ(out[0], 0xFFu);
}

TEST(Base32decLeniency, LowercaseRejectedWhenStrict) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec(out, sizeof(out), "zw", false), (size_t)-1);
}

TEST(Base32decLeniency, AmbiguityMappingsWhenLenient) {
  unsigned char a[16] = {0}, b[16] = {0};
  const size_t ref = base32dec(b, sizeof(b), "11111111", true);
  ASSERT_NE(ref, (size_t)-1);

  const size_t al = base32dec(a, sizeof(a), "I1111111", true);
  EXPECT_EQ(al, ref);
  EXPECT_EQ(memcmp(a, b, ref), 0);

  const size_t ll = base32dec(a, sizeof(a), "L1111111", true);
  EXPECT_EQ(ll, ref);
  EXPECT_EQ(memcmp(a, b, ref), 0);

  const size_t ol = base32dec(a, sizeof(a), "O0000000", true);
  const size_t zl = base32dec(b, sizeof(b), "00000000", true);
  EXPECT_EQ(ol, zl);
  EXPECT_EQ(memcmp(a, b, ol), 0);
}

TEST(Base32decLeniency, AmbiguityRejectedWhenStrict) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec(out, sizeof(out), "I1111111", false), (size_t)-1);
  EXPECT_EQ(base32dec(out, sizeof(out), "L1111111", false), (size_t)-1);
  EXPECT_EQ(base32dec(out, sizeof(out), "O0000000", false), (size_t)-1);
}

TEST(Base32decLeniency, PaddingAcceptedWhenLenient) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec(out, sizeof(out), "ZW======", true), 1u);
  EXPECT_EQ(out[0], 0xFFu);
}

TEST(Base32decLeniency, PaddingRejectedWhenStrict) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec(out, sizeof(out), "ZW======", false), (size_t)-1);
}

TEST(Base32decLeniency, HyphenSkippedWhenLenient) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec(out, sizeof(out), "CSQP-YRK1E8", true), 6u);
  EXPECT_EQ(memcmp(out, "foobar", 6), 0);
}

TEST(Base32decLeniency, HyphenRejectedWhenStrict) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec(out, sizeof(out), "CSQP-YRK1E8", false), (size_t)-1);
}

TEST(Base32decLeniency, StrictDecodesCanonicalUppercase) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec(out, sizeof(out), "CSQPYRK1E8", false), 6u);
  EXPECT_EQ(memcmp(out, "foobar", 6), 0);
}

// ════════════════════════════════════════════════════════════════════════════
// base32dec_ex — flags and legacy-semantics emulation
// ════════════════════════════════════════════════════════════════════════════

TEST(Base32decEx, LegacyFlagsEmulateOldSemantics) {
  const uint32_t legacy = BASE32DEC_FLAG_ACCEPT_PADDING | BASE32DEC_FLAG_PARTIAL_ON_ERR;
  unsigned char out[16] = {0};

  // Strict: lowercase rejected (partial-on-error yields 0, not (size_t)-1).
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "zw", legacy), 0u);

  // Strict: '=' padding accepted (clean terminator).
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "ZW======", legacy), 1u);
  EXPECT_EQ(out[0], 0xFFu);

  // Strict: I/L/O rejected (partial-on-error yields 0).
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "I1111111", legacy), 0u);

  // Strict: canonical uppercase still decodes.
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "CSQPYRK1E8", legacy), 6u);
  EXPECT_EQ(memcmp(out, "foobar", 6), 0);
}

TEST(Base32decEx, PartialOnErrorReturnsBytesSoFar) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "ZW!", BASE32DEC_FLAG_PARTIAL_ON_ERR), 1u);
  EXPECT_EQ(out[0], 0xFFu);
  // Without PARTIAL_ON_ERR the same input is a hard error.
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "ZW!", 0), (size_t)-1);
}

TEST(Base32decEx, AcceptPaddingFlag) {
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "ZW======", BASE32DEC_FLAG_ACCEPT_PADDING), 1u);
  EXPECT_EQ(out[0], 0xFFu);
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "ZW======", 0), (size_t)-1);
}

TEST(Base32decEx, LenientFlagAlone) {
  // LENIENT without ACCEPT_PADDING: case-insensitive but '=' still rejected.
  unsigned char out[16] = {0};
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "zw", BASE32DEC_FLAG_LENIENT), 1u);
  EXPECT_EQ(out[0], 0xFFu);
  EXPECT_EQ(base32dec_ex(out, sizeof(out), "ZW======", BASE32DEC_FLAG_LENIENT), (size_t)-1);
}

// ════════════════════════════════════════════════════════════════════════════
// Size helpers — contract against first-principles formulas
// ════════════════════════════════════════════════════════════════════════════

TEST(Base32encsize, MatchesCeilFormula) {
  for (size_t n = 0; n <= 100; ++n) {
    const size_t want = (n * 8 + 4) / 5;   // ceil(n*8/5)
    EXPECT_EQ(base32encsize(n), want) << "n=" << n;
  }
}

TEST(Base32decsize, MatchesFloorFormula) {
  // floor(count*5/8) is the number of decoded bytes.  The implementation
  // returns 0 or 1 instead.
  for (size_t n = 0; n <= 100; ++n) {
    const size_t want = (n * 5) / 8;
    EXPECT_EQ(base32decsize(n), want) << "n=" << n;
  }
}

TEST(Base32ProvideEncodedBufferSize, SufficientUpperBound) {
  // The provided size must never be smaller than the exact encoded length.
  for (size_t n = 0; n <= 1000; ++n) {
    EXPECT_GE(Base32ProvideEncodedBufferSize(n), base32encsize(n)) << "n=" << n;
  }
}
