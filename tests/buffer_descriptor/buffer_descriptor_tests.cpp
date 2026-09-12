/**
 * @file buffer_descriptor_tests.cpp
 * @brief Adversarial test suite for the BufferDescriptor module.
 *
 * The BufferDescriptor API surface is three functions:
 *   - BufferDescriptorInit(BufferDescriptor*, size_t initial_cap)
 *   - BufferDescriptorRelease(BufferDescriptor*)
 *   - BufferDescriptorAppendFormatted(BufferDescriptor*, const char* fmt, ...)
 *
 * This suite pairs ordinary contract tests with deliberately hostile inputs —
 * NULL pointers, huge widths, exact capacity boundaries, overflow attempts and
 * malformed/undefined arguments — in order to break the code and surface hidden
 * flaws rather than merely record its current behaviour.
 *
 * Copyright (C) 2015-2026  unfacd works
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

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

extern "C" {
#include "uflib/buffer_descriptor/buffer_descriptor.h"
}

namespace {

// BUFFER_DESCRIPTOR_MAX_BYTES is a *private* #define in buffer_descriptor.c
// (default 1024*1024) and is not visible to consumers.  This suite hard-codes
// the default so it can exercise the capacity cap exactly; if the private
// default is ever changed, update this constant in lock-step.
constexpr size_t kMaxBytes   = 1024u * 1024u;   // 1 MiB
constexpr size_t kMaxContent = kMaxBytes - 1u;  // largest legal content length

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// Init — contract and boundary clamping
// ════════════════════════════════════════════════════════════════════════════

TEST(BufferDescriptorInit, DefaultsToMinimumCapacity) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 0);
  ASSERT_NE(bd.data, nullptr);
  EXPECT_EQ(bd.size, 0u);
  EXPECT_EQ(bd.size_max, 64u);
  EXPECT_EQ(bd.data[0], '\0');
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorInit, ClampsBelowMinimumTo64) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 1);
  EXPECT_EQ(bd.size_max, 64u);
  BufferDescriptorRelease(&bd);

  BufferDescriptorInit(&bd, 63);
  EXPECT_EQ(bd.size_max, 64u);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorInit, PreservesInRangeCapacityExactly) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 65);   // non-power-of-two, in range
  EXPECT_EQ(bd.size_max, 65u);
  BufferDescriptorRelease(&bd);

  BufferDescriptorInit(&bd, kMaxBytes);
  EXPECT_EQ(bd.size_max, kMaxBytes);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorInit, ClampsAboveMaximumToOneMebibyte) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, std::numeric_limits<size_t>::max());
  EXPECT_EQ(bd.size_max, kMaxBytes);
  BufferDescriptorRelease(&bd);

  BufferDescriptorInit(&bd, kMaxBytes + 1u);
  EXPECT_EQ(bd.size_max, kMaxBytes);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorInit, NullDescriptorIsNoop) {
  // Must not crash; the API silently ignores a NULL descriptor.
  BufferDescriptorInit(nullptr, 64);
  SUCCEED();
}

// ════════════════════════════════════════════════════════════════════════════
// Release — lifecycle and reuse
// ════════════════════════════════════════════════════════════════════════════

TEST(BufferDescriptorRelease, NullsAllFields) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 128);
  ASSERT_NE(bd.data, nullptr);

  BufferDescriptorRelease(&bd);
  EXPECT_EQ(bd.data, nullptr);
  EXPECT_EQ(bd.size, 0u);
  EXPECT_EQ(bd.size_max, 0u);
}

TEST(BufferDescriptorRelease, NullDescriptorIsNoop) {
  BufferDescriptorRelease(nullptr);
  SUCCEED();
}

TEST(BufferDescriptorRelease, DoubleReleaseIsSafe) {
  // Second release must be a harmless free(NULL); exposes any use-after-free
  // or missing NULL-reset in the implementation.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  BufferDescriptorRelease(&bd);
  BufferDescriptorRelease(&bd);
  EXPECT_EQ(bd.data, nullptr);
  EXPECT_EQ(bd.size, 0u);
  EXPECT_EQ(bd.size_max, 0u);
}

TEST(BufferDescriptorRelease, RecycleLoop) {
  for (int i = 0; i < 1000; ++i) {
    BufferDescriptor bd;
    BufferDescriptorInit(&bd, 64);
    EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "cycle-%d", i), 0);
    BufferDescriptorRelease(&bd);
  }
  SUCCEED();
}

// ════════════════════════════════════════════════════════════════════════════
// AppendFormatted — normal contract
// ════════════════════════════════════════════════════════════════════════════

TEST(BufferDescriptorAppendFormatted, SimpleString) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "hello"), 0);
  EXPECT_EQ(bd.size, 5u);
  EXPECT_STREQ(bd.data, "hello");
  EXPECT_EQ(bd.data[bd.size], '\0');
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, AppendsAndTerminates) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "abc"), 0);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "def"), 0);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "ghi"), 0);
  EXPECT_EQ(bd.size, 9u);
  EXPECT_STREQ(bd.data, "abcdefghi");
  EXPECT_EQ(bd.data[bd.size], '\0');
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, MixedFormatsMatchSnprintf) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  const char *fmt = "%d|%s|%c|%.2f|0x%x|%zu|%%";
  int rc = BufferDescriptorAppendFormatted(
      &bd, fmt, -42, "hello", 'Z', 3.14159, 255u, (size_t)123456789);
  char ref[256];
  snprintf(ref, sizeof(ref), fmt, -42, "hello", 'Z', 3.14159, 255u,
           (size_t)123456789);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(bd.size, strlen(ref));
  EXPECT_STREQ(bd.data, ref);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, EmptyFormatIsNoop) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, ""), 0);
  EXPECT_EQ(bd.size, 0u);
  EXPECT_EQ(bd.data[0], '\0');
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, PrecisionTruncation) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "%.3s", "abcdef"), 0);
  EXPECT_EQ(bd.size, 3u);
  EXPECT_STREQ(bd.data, "abc");
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, GrowthDoublesCapacityFrom64) {
  // 100 bytes into a 64-byte buffer must grow to 128 (doubling).
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(bd.size_max, 64u);
  std::string payload(100, 'x');
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "%s", payload.c_str()), 0);
  EXPECT_EQ(bd.size, 100u);
  EXPECT_EQ(bd.size_max, 128u);
  EXPECT_EQ(memcmp(bd.data, payload.c_str(), 100), 0);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, GrowthDoublesNonPowerOfTwoCapacity) {
  // A non-power-of-two initial capacity (100) doubles to 200 on overflow.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 100);
  EXPECT_EQ(bd.size_max, 100u);
  std::string payload(150, 'y');
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "%s", payload.c_str()), 0);
  EXPECT_EQ(bd.size_max, 200u);
  EXPECT_EQ(bd.size, 150u);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, GrowthFromNonPowerOfTwoJumpsToCap) {
  // A non-power-of-two capacity above BUFFER_DESCRIPTOR_MAX_BYTES/2 cannot
  // double again without exceeding the cap, so the growth loop must clamp
  // straight to kMaxBytes (the `ncap = MAX; break` branch).  Exercised only
  // when the *starting* capacity is a non-power-of-two near the ceiling.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 600000);          // in [64, 1 MiB], not a power of 2
  EXPECT_EQ(bd.size_max, 600000u);

  std::string payload(700000, 'g');           // forces growth past the cap barrier
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "%s", payload.c_str()), 0);
  EXPECT_EQ(bd.size, 700000u);
  EXPECT_EQ(bd.size_max, kMaxBytes);          // clamped straight to the hard cap
  EXPECT_EQ(memcmp(bd.data, payload.c_str(), 700000), 0);
  EXPECT_EQ(bd.data[700000], '\0');

  // The buffer now holds 700000 of its 1 MiB capacity — still usable, not full.
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "z"), 0);
  EXPECT_EQ(bd.size, 700001u);
  EXPECT_EQ(bd.data[700000], 'z');
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, AccumulatesAcrossManyGrowths) {
  // Repeated small appends must cross many doubling boundaries while preserving
  // every byte in order and maintaining the NUL invariant at every step.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  std::string expected;
  for (int i = 0; i < 500; ++i) {
    char chunk[16];
    snprintf(chunk, sizeof(chunk), "[%03d]", i);
    EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "%s", chunk), 0);
    expected += chunk;
    ASSERT_EQ(bd.size, expected.size()) << "size mismatch at iteration " << i;
    ASSERT_EQ(bd.data[bd.size], '\0') << "missing NUL at iteration " << i;
  }
  EXPECT_EQ(bd.size, expected.size());
  EXPECT_EQ(memcmp(bd.data, expected.c_str(), expected.size()), 0);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, NullDescriptorReturnsError) {
  EXPECT_EQ(BufferDescriptorAppendFormatted(nullptr, "x"), BUFFER_DESCRIPTOR_ERR_NULL_ARG);
}

// ════════════════════════════════════════════════════════════════════════════
// AppendFormatted — adversarial inputs
// ════════════════════════════════════════════════════════════════════════════

TEST(BufferDescriptorAppendFormatted, HugeWidthExceedsCapReturnsError) {
  // A format width that expands beyond the 1 MiB cap must be rejected cleanly
  // (return -1) and leave the buffer untouched — no partial write, no growth.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  const char *before = bd.data;
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "%2000000d", 42), BUFFER_DESCRIPTOR_ERR_CAP);
  EXPECT_EQ(bd.size, 0u);
  EXPECT_EQ(bd.data, before);        // no realloc happened
  EXPECT_EQ(bd.data[0], '\0');
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, IntMaxWidthReturnsError) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "%999999999d", 42), BUFFER_DESCRIPTOR_ERR_CAP);
  EXPECT_EQ(bd.size, 0u);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, FillToExactCapacityThenOneMoreByteFails) {
  // The cap is `size + needed + 1 <= kMaxBytes`, so the largest admissible
  // content is kMaxContent = kMaxBytes - 1.  Appending exactly that much must
  // succeed; a single extra byte must fail and leave content intact.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  std::string big(kMaxContent, 'a');
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "%s", big.c_str()), 0);
  EXPECT_EQ(bd.size, kMaxContent);
  EXPECT_EQ(bd.size_max, kMaxBytes);
  EXPECT_EQ(memcmp(bd.data, big.c_str(), kMaxContent), 0);
  EXPECT_EQ(bd.data[kMaxContent], '\0');

  // One more byte overflows the cap.
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "x"), BUFFER_DESCRIPTOR_ERR_CAP);
  EXPECT_EQ(bd.size, kMaxContent);
  EXPECT_EQ(memcmp(bd.data, big.c_str(), kMaxContent), 0);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, EmptyAppendAtFullCapacitySucceeds) {
  // At the exact cap boundary, an empty append (needed == 0) must be a no-op
  // success, not a spurious overflow error.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  std::string big(kMaxContent, 'b');
  ASSERT_EQ(BufferDescriptorAppendFormatted(&bd, "%s", big.c_str()), 0);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, ""), 0);
  EXPECT_EQ(bd.size, kMaxContent);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, ZeroInitializedStructWorksWithoutInit) {
  // A value-initialized (all-zero) descriptor — data=NULL, size=0, size_max=0 —
  // must not crash and must self-provision via realloc(NULL, …).  This probes
  // whether the module assumes Init was called.
  BufferDescriptor bd{};
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "probe"), 0);
  EXPECT_STREQ(bd.data, "probe");
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, AppendAfterReleaseReinitializes) {
  // After Release, append must self-provision from a fully zeroed descriptor.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "one"), 0);
  BufferDescriptorRelease(&bd);
  ASSERT_EQ(bd.data, nullptr);

  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "two"), 0);
  EXPECT_STREQ(bd.data, "two");
  EXPECT_EQ(bd.size, 3u);
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, NullStringArgument) {
  // %s with a NULL argument is undefined by the C standard; glibc renders
  // "(null)".  This test asserts the module forwards the graceful glibc
  // behaviour (return 0, terminated) rather than crashing.  It is glibc-
  // specific and should be revisited on other libc implementations.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, "%s", (const char *)nullptr), 0);
  EXPECT_EQ(bd.size, 6u);
  EXPECT_EQ(bd.data[bd.size], '\0');
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorAppendFormatted, NullFormatStringReturnsError) {
  // NULL `fmt` must be rejected before vsnprintf dereferences it
  // (BUFFER_DESCRIPTOR_ERR_NULL_ARG, buffer untouched). Previously unguarded,
  // this path SIGSEGV'd.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  EXPECT_EQ(BufferDescriptorAppendFormatted(&bd, nullptr), BUFFER_DESCRIPTOR_ERR_NULL_ARG);
  EXPECT_EQ(bd.size, 0u);
  EXPECT_EQ(bd.data[0], '\0');
  BufferDescriptorRelease(&bd);
}

// ════════════════════════════════════════════════════════════════════════════
// Invariants — structural consistency
// ════════════════════════════════════════════════════════════════════════════

TEST(BufferDescriptorInvariants, SizeNeverExceedsCapacity) {
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 64);
  std::string payload(kMaxBytes / 2, 'z');
  ASSERT_EQ(BufferDescriptorAppendFormatted(&bd, "%s", payload.c_str()), 0);
  EXPECT_LE(bd.size, bd.size_max);
  EXPECT_EQ(bd.data[bd.size], '\0');
  BufferDescriptorRelease(&bd);
}

TEST(BufferDescriptorInvariants, SizeIsContentLength) {
  // size tracks content length (not allocated capacity); size_max tracks the
  // allocated capacity.  Documented here to lock the public contract.
  BufferDescriptor bd;
  BufferDescriptorInit(&bd, 4096);
  EXPECT_EQ(bd.size, 0u);
  EXPECT_GE(bd.size_max, 4096u);
  BufferDescriptorAppendFormatted(&bd, "%s", "1234567890");
  EXPECT_EQ(bd.size, 10u);
  EXPECT_EQ(bd.size_max, 4096u);   // no growth needed
  BufferDescriptorRelease(&bd);
}
