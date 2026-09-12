/**
 * @file ufsrvuid_tests.cpp
 * @brief Adversarial test suite for src/ufsrvuid.c (Crockford Base32 ULID codec).
 *
 * The tests are written adversarially: they feed malicious, edge-case and
 * unexpected inputs designed to break the codec and trap silent corruption.
 * Round-trip tests assert the mathematical inverse; validation/overflow tests
 * assert the *fixed* invariant (rejection / clamping), never the buggy output.
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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <atomic>
#include <type_traits>

#include "gtest/gtest.h"

// ufsrvuid.h pulls in <uflib/standard_c_includes.h>, which (under C++) includes
// <atomic> and defines _Atomic(X).  <atomic> must therefore be included before
// the extern "C" block so its include guard is already set and no template
// declaration is textually nested inside the C-linkage block.
extern "C" {
#include <uflib/ufsrvuid.h>
}

namespace {

// Crockford's Base32 alphabet excludes I, L, O, U.  Returns true iff `c` is a
// legal symbol in the 26-char encoded form.
bool sIsCrockford(char c)
{
  if (c >= '0' && c <= '9') return true;
  if (c >= 'A' && c <= 'Z') return c != 'I' && c != 'L' && c != 'O' && c != 'U';
  return false;
}

// Deterministic xorshift64* PRNG so the fuzz corpus is reproducible run-to-run.
struct Xorshift64 {
  uint64_t state;
  explicit Xorshift64(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
  uint64_t next()
  {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
  }
};

// The custom epoch baked into src/ufsrvuid.c (private static — duplicated here
// because the value is not exposed through any public header).
const long long kCustomEpochMillis = 1401277473000LL;

}  // namespace

// UfsrvUid is a bare uint8_t[16] with alignof 1 — it carries no alignment
// guarantee.  The accessors in src/ufsrvuid.c therefore read/write it with
// memcpy rather than type-punned casts (see audit doc TD-006).
static_assert(alignof(UfsrvUid) == 1,
              "UfsrvUid carries no alignment guarantee");
static_assert(sizeof(UfsrvUid) == 16,
              "UfsrvUid must remain 16 bytes (Crockford Base32 of 128 bits)");

// ── Round-trip correctness (bijection) ──────────────────────────────────────
// These assert the mathematical inverse relationship between encode and decode.
// A broken codec (bit-shift off-by-one, dropped bits, non-injective packing)
// fails these; they do not pin down any specific buggy output.

TEST(UfsrvUidRoundTrip, EncodeDecodeIsBijectionOverRandomInputs)
{
  Xorshift64 rng(0xCAFE1234ULL);
  for (int iter = 0; iter < 10000; ++iter) {
    UfsrvUid a = {};
    for (int i = 0; i < 16; ++i) a.data[i] = static_cast<uint8_t>(rng.next());

    char enc[27];
    memset(enc, 0, sizeof(enc));
    UfsrvUidConvertSerialise(&a, enc);

    for (int i = 0; i < 26; ++i) {
      ASSERT_TRUE(sIsCrockford(enc[i])) << "byte " << i << " of iteration " << iter;
    }

    UfsrvUid b = {};
    UfsrvUid *r = UfsrvUidCreateFromEncodedText(enc, &b);
    ASSERT_NE(r, nullptr) << "valid encoding rejected at iteration " << iter;
    EXPECT_EQ(memcmp(a.data, b.data, 16), 0) << "mismatch at iteration " << iter;
  }
}

TEST(UfsrvUidRoundTrip, PerByteBitMappingHasNoCrossTalk)
{
  const uint8_t probes[] = {
      0x00, 0x01, 0x07, 0x08, 0x0F, 0x10, 0x1F, 0x20,
      0x3F, 0x40, 0x7F, 0x80, 0x9F, 0xC0, 0xE0, 0xFF,
  };
  for (int pos = 0; pos < 16; ++pos) {
    for (uint8_t v : probes) {
      UfsrvUid a = {};
      a.data[pos] = v;

      char enc[27];
      memset(enc, 0, sizeof(enc));
      UfsrvUidConvertSerialise(&a, enc);

      UfsrvUid b = {};
      UfsrvUidCreateFromEncodedText(enc, &b);

      for (int i = 0; i < 16; ++i) {
        if (i == pos) {
          EXPECT_EQ(b.data[i], v) << "pos " << pos << " value " << (unsigned)v;
        } else {
          EXPECT_EQ(b.data[i], 0) << "cross-talk into pos " << i << " from pos " << pos;
        }
      }
    }
  }
}

// ── Field layout: intended contract for valid inputs ────────────────────────

TEST(UfsrvUidFields, GenerateRoundTripsValidFields)
{
  UfsrvUidGeneratorDescriptor d = {};
  d.instance_id = 12345;
  d.timestamp = kCustomEpochMillis + 99999;  // delta 99999 ms
  d.uid = 0xDEADBEEFCAFEBABEUL;
  UfsrvUid u = {};
  UfsrvUid *r = UfsrvUidGenerate(&d, &u);

  EXPECT_EQ(r, &u);
  EXPECT_EQ(UfsrvUidGetInstanceId(&u), 12345u);
  EXPECT_EQ(UfsrvUidGetTimestamp(&u), 99999UL);
  EXPECT_EQ(UfsrvUidGetSequenceId(&u), 0xDEADBEEFCAFEBABEUL);
}

TEST(UfsrvUidFields, GenerateAllocatesWhenOutputIsNull)
{
  UfsrvUidGeneratorDescriptor d = {};
  d.instance_id = 1;
  d.timestamp = kCustomEpochMillis;
  d.uid = 42;
  UfsrvUid *u = UfsrvUidGenerate(&d, nullptr);

  ASSERT_NE(u, nullptr);
  EXPECT_EQ(UfsrvUidGetSequenceId(u), 42UL);
  free(u);
}

// ── System user ─────────────────────────────────────────────────────────────

TEST(UfsrvUidSystemUser, DecodedSystemUserHasSequenceZero)
{
  UfsrvUid u = {};
  UfsrvUidCreateFromEncodedText(UFSRV_SYSTEMUSER_UID, &u);

  EXPECT_EQ(UfsrvUidGetSequenceId(&u), 0UL);
  EXPECT_EQ(UfsrvUidGetInstanceId(&u), 1u);
  EXPECT_EQ(UfsrvUidGetTimestamp(&u), 0UL);
  EXPECT_TRUE(UfsrvUidIsSystemUser(&u));
}

TEST(UfsrvUidSystemUser, IsSystemUserRejectsAnyByteDeviation)
{
  for (int pos = 0; pos < 16; ++pos) {
    UfsrvUid u = {};
    UfsrvUidCreateFromEncodedText(UFSRV_SYSTEMUSER_UID, &u);
    u.data[pos] ^= 0x01;
    EXPECT_FALSE(UfsrvUidIsSystemUser(&u)) << "byte " << pos;
  }
}

TEST(UfsrvUidSystemUser, RawSystemUserReturnsConstSharedSingleton)
{
  // TD-008: the raw system user is returned as pointer-to-const, so the shared
  // static cannot be corrupted through the public API.
  static_assert(std::is_same<decltype(UfsrvUidRawSystemUser()), const UfsrvUid *>::value,
                "UfsrvUidRawSystemUser() must return a const UfsrvUid*");
  static_assert(std::is_same<decltype(UfsrvUidRawDataSystemUser()), const uint8_t *>::value,
                "UfsrvUidRawDataSystemUser() must return a const uint8_t*");

  const UfsrvUid *sys = UfsrvUidRawSystemUser();
  ASSERT_NE(sys, nullptr);
  EXPECT_TRUE(UfsrvUidIsSystemUser(sys));

  const uint8_t *raw = UfsrvUidRawDataSystemUser();
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(memcmp(sys->data, raw, 16), 0);
}

// ── Basic accessors ─────────────────────────────────────────────────────────

TEST(UfsrvUidBasic, CopyAndIsEqualAreConsistent)
{
  UfsrvUid a = {}, b = {};
  UfsrvUidCreateFromEncodedText(UFSRV_SYSTEMUSER_UID, &a);
  UfsrvUidCopy(&a, &b);
  EXPECT_TRUE(UfsrvUidIsEqual(&a, &b));

  b.data[15] ^= 0x80;
  EXPECT_FALSE(UfsrvUidIsEqual(&a, &b));
}

// ── Adversarial: input validation (TD-001/002/003) ──────────────────────────

TEST(UfsrvUidAdversarial, DecodeRejectsForbiddenCharacters)
{
  // Control: a canonical string (leading char in 0..7) still round-trips.
  {
    char input[27] = "00000000000000000000000000";
    input[0] = '7';
    UfsrvUid u = {};
    UfsrvUid *r = UfsrvUidCreateFromEncodedText(input, &u);
    ASSERT_NE(r, nullptr);
    char back[27];
    memset(back, 0, sizeof(back));
    UfsrvUidConvertSerialise(&u, back);
    EXPECT_EQ(memcmp(back, input, 26), 0);
  }

  const char forbidden[] = {'I', 'L', 'O', 'U', 'i', 'l', 'o', 'u', '-', '='};
  for (char c : forbidden) {
    char input[27] = "00000000000000000000000000";
    input[0] = c;
    UfsrvUid u = {};
    u.data[0] = 0xAA;  // sentinel: decode must not touch output on rejection
    EXPECT_EQ(UfsrvUidCreateFromEncodedText(input, &u), nullptr)
        << "forbidden char '" << c << "' should be rejected";
    EXPECT_EQ(u.data[0], 0xAA) << "output must be untouched on rejection";
  }
}

TEST(UfsrvUidAdversarial, DecodeRejectsNonCanonicalLeadingCharacter)
{
  // The leading char carries only 3 bits (0..7); '8'..'Z' must be rejected.
  const char noncanonical[] = {'8', '9', 'A', 'Z'};
  for (char c : noncanonical) {
    char input[27] = "00000000000000000000000000";
    input[0] = c;
    UfsrvUid u = {};
    EXPECT_EQ(UfsrvUidCreateFromEncodedText(input, &u), nullptr)
        << "leading char '" << c << "' should be rejected";
  }
}

TEST(UfsrvUidAdversarial, DecodeRejectsShortInput)
{
  char shortbuf[2] = {'0', '\0'};
  UfsrvUid u = {};
  EXPECT_EQ(UfsrvUidCreateFromEncodedText(shortbuf, &u), nullptr);
}

// ── Adversarial: field clamping (TD-004/005) ────────────────────────────────

TEST(UfsrvUidAdversarial, GenerateMasksInstanceIdTo23Bits)
{
  UfsrvUidGeneratorDescriptor d = {};
  d.timestamp = kCustomEpochMillis;  // delta 0
  d.instance_id = 0x80000000u;       // bit 31 set — must be masked to 23 bits
  d.uid = 123;
  UfsrvUid u = {};
  UfsrvUidGenerate(&d, &u);

  EXPECT_EQ(UfsrvUidGetInstanceId(&u), 0u);
  EXPECT_EQ(UfsrvUidGetTimestamp(&u), 0UL);  // no leak into the timestamp field
}

TEST(UfsrvUidAdversarial, GenerateClampsPreEpochTimestamp)
{
  UfsrvUidGeneratorDescriptor d = {};
  d.timestamp = 0;     // long before the custom epoch
  d.instance_id = 0;
  d.uid = 0;
  UfsrvUid u = {};
  UfsrvUidGenerate(&d, &u);

  EXPECT_EQ(UfsrvUidGetTimestamp(&u), 0UL);  // clamped, not wrapped
}

TEST(UfsrvUidAdversarial, GenerateClampsTimestampTo41Bits)
{
  UfsrvUidGeneratorDescriptor d = {};
  d.timestamp = kCustomEpochMillis + (1LL << 42);  // delta > 2^41
  d.instance_id = 0;
  d.uid = 0;
  UfsrvUid u = {};
  UfsrvUidGenerate(&d, &u);

  EXPECT_EQ(UfsrvUidGetTimestamp(&u), 0x1FFFFFFFFFFUL);  // 2^41 - 1
}

// ── Serialise write footprint (TD-007: documented, 26 bytes, no NUL) ────────

TEST(UfsrvUidAdversarial, SerialiseWrites26CharsAndAlwaysNulTerminates)
{
  UfsrvUid u = {};
  u.data[0] = 0xFF;
  u.data[15] = 0x01;

  char buf[28];
  memset(buf, 0xAA, sizeof(buf));  // canary fill

  char *out = UfsrvUidConvertSerialise(&u, buf);
  EXPECT_EQ(out, buf);

  // 26 chars + NUL at [26]; byte [27] is the untouched canary.
  EXPECT_EQ(buf[26], '\0');
  EXPECT_EQ(static_cast<unsigned char>(buf[27]), 0xAAu);
  EXPECT_EQ(strlen(buf), 26u);
}

TEST(UfsrvUidAdversarial, SerialiseAllocPathIsAlsoNulTerminated)
{
  UfsrvUid u = {};
  char *heap = UfsrvUidConvertSerialise(&u, nullptr);
  ASSERT_NE(heap, nullptr);
  EXPECT_EQ(heap[26], '\0');
  EXPECT_EQ(strlen(heap), 26u);
  free(heap);
}

// ── Sequence id from encoded ────────────────────────────────────────────────

TEST(UfsrvUidSequenceFromEncoded, RejectsWrongLengthsAndDecodesValid)
{
  UfsrvUidGeneratorDescriptor d = {};
  d.instance_id = 7;
  d.timestamp = kCustomEpochMillis;
  d.uid = 0x0123456789ABCDEFUL;
  UfsrvUid u = {};
  UfsrvUidGenerate(&d, &u);

  char enc[27];
  memset(enc, 0, sizeof(enc));
  UfsrvUidConvertSerialise(&u, enc);
  EXPECT_EQ(UfsrvUidGetSequenceIdFromEncoded(enc), 0x0123456789ABCDEFUL);

  char short25[26];
  memset(short25, '0', 25);
  short25[25] = '\0';
  EXPECT_EQ(UfsrvUidGetSequenceIdFromEncoded(short25), ULONG_MAX);

  char long27[28];
  memset(long27, '0', 27);
  long27[27] = '\0';
  EXPECT_EQ(UfsrvUidGetSequenceIdFromEncoded(long27), ULONG_MAX);
}

TEST(UfsrvUidDecodeSpecific, SequenceIdOfKnownEncoding)
{
  const char *encoded = "3J140H9YY5H43KM08000000000";
  UfsrvUid u = {};
  UfsrvUidCreateFromEncodedText(encoded, &u);

  const unsigned long seq = UfsrvUidGetSequenceId(&u);

  // Output the sequence id so it is visible when the test runs.
  std::printf("sequence id of %s = %lu (0x%lx)\n", encoded, seq, seq);

  EXPECT_EQ(seq, 314UL);
}

// ── NULL safety (TD-009) ────────────────────────────────────────────────────

TEST(UfsrvUidNullSafety, NullInputsAreGraceful)
{
  UfsrvUid u = {};
  EXPECT_EQ(UfsrvUidCreateFromEncodedText(nullptr, &u), nullptr);
  EXPECT_EQ(UfsrvUidCreateFromBinary(nullptr, &u), nullptr);
  EXPECT_EQ(UfsrvUidGenerate(nullptr, &u), nullptr);
  EXPECT_EQ(UfsrvUidConvertToBinary(&u, nullptr), nullptr);
  EXPECT_EQ(UfsrvUidConvertSerialise(nullptr, nullptr), nullptr);
  EXPECT_EQ(UfsrvUidGetSequenceIdFromEncoded(nullptr), ULONG_MAX);
  EXPECT_EQ(UfsrvUidGetSequenceId(nullptr), 0UL);
  EXPECT_EQ(UfsrvUidGetInstanceId(nullptr), 0u);
  EXPECT_EQ(UfsrvUidGetTimestamp(nullptr), 0UL);
  EXPECT_FALSE(UfsrvUidIsEqual(nullptr, &u));
  EXPECT_FALSE(UfsrvUidIsEqual(&u, nullptr));
  EXPECT_TRUE(UfsrvUidIsEqual(nullptr, nullptr));
  EXPECT_FALSE(UfsrvUidIsSystemUser(nullptr));

  UfsrvUidCopy(nullptr, &u);  // no-op, must not crash
  UfsrvUidCopy(&u, nullptr);  // no-op, must not crash
}

// ── Binary codec (TD-011) ───────────────────────────────────────────────────

TEST(UfsrvUidBinary, CreateFromBinaryAndConvertToBinaryRoundTrip)
{
  const uint8_t bytes[16] = {
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
      0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
  };

  UfsrvUid u = {};
  UfsrvUid *r = UfsrvUidCreateFromBinary(bytes, &u);
  ASSERT_EQ(r, &u);
  EXPECT_EQ(memcmp(u.data, bytes, 16), 0);

  uint8_t out[16] = {};
  uint8_t *o = UfsrvUidConvertToBinary(&u, out);
  ASSERT_EQ(o, out);
  EXPECT_EQ(memcmp(out, bytes, 16), 0);

  UfsrvUid *alloc = UfsrvUidCreateFromBinary(bytes, nullptr);
  ASSERT_NE(alloc, nullptr);
  EXPECT_EQ(memcmp(alloc->data, bytes, 16), 0);
  free(alloc);
}
