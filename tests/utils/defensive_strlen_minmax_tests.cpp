#include "gtest/gtest.h"

#include <cstring>

extern "C" {
#include "uflib/utils_str.h"
}

// — Normal conditions ————————————————————————————————————————————

TEST(DefensiveStrlenWithMinMax, StringWithinRange) {
  size_t out_len = 0;
  bool ret = DefensiveStrlenWithMinMax("hello", 1, 10, &out_len);
  EXPECT_TRUE(ret);
  EXPECT_EQ(out_len, 5u);
}

TEST(DefensiveStrlenWithMinMax, StringWithinRangeNoOutLen) {
  bool ret = DefensiveStrlenWithMinMax("hello", 1, 10, nullptr);
  EXPECT_TRUE(ret);
}

TEST(DefensiveStrlenWithMinMax, StringAtExactMinBoundary) {
  size_t out_len = 0;
  // "hi" has length 2, min_sz = 2
  bool ret = DefensiveStrlenWithMinMax("hi", 2, 10, &out_len);
  EXPECT_TRUE(ret);
  EXPECT_EQ(out_len, 2u);
}

TEST(DefensiveStrlenWithMinMax, StringAtExactMaxBoundary) {
  size_t out_len = 0;
  // "ab" has length 2, max_sz = 2 — but NUL at position 2 is NOT found
  // because the loop condition is (len < max_sz), i.e. max_sz is exclusive
  bool ret = DefensiveStrlenWithMinMax("ab", 0, 2, &out_len);
  EXPECT_FALSE(ret);
  EXPECT_EQ(out_len, 2u); // scanned max_sz bytes without finding NUL
}

// — Boundary: invalid parameters ————————————————————————————————

TEST(DefensiveStrlenWithMinMax, MaxSzZero) {
  size_t out_len = 42;
  bool ret = DefensiveStrlenWithMinMax("hello", 0, 0, &out_len);
  EXPECT_FALSE(ret);
  EXPECT_EQ(out_len, 0u);
}

TEST(DefensiveStrlenWithMinMax, MinGreaterThanMax) {
  size_t out_len = 42;
  bool ret = DefensiveStrlenWithMinMax("hello", 10, 5, &out_len);
  EXPECT_FALSE(ret);
  EXPECT_EQ(out_len, 0u);
}

TEST(DefensiveStrlenWithMinMax, MaxSzZeroNoOutLen) {
  bool ret = DefensiveStrlenWithMinMax("hello", 0, 0, nullptr);
  EXPECT_FALSE(ret);
}

TEST(DefensiveStrlenWithMinMax, MinGreaterThanMaxNoOutLen) {
  bool ret = DefensiveStrlenWithMinMax("hello", 10, 5, nullptr);
  EXPECT_FALSE(ret);
}

// — Boundary: string too short ————————————————————————————————————

TEST(DefensiveStrlenWithMinMax, StringTooShort) {
  size_t out_len = 0;
  // "hi" has length 2, requires min 5
  bool ret = DefensiveStrlenWithMinMax("hi", 5, 10, &out_len);
  EXPECT_FALSE(ret);
  EXPECT_EQ(out_len, 2u); // actual length found
}

TEST(DefensiveStrlenWithMinMax, StringTooShortNoOutLen) {
  bool ret = DefensiveStrlenWithMinMax("hi", 5, 10, nullptr);
  EXPECT_FALSE(ret);
}

// — Boundary: string too long (no NUL within max_sz) ————————————

TEST(DefensiveStrlenWithMinMax, StringExceedsMax) {
  size_t out_len = 0;
  // "hello" has length 5 but max_sz is only 3 — no NUL within first 3 bytes
  bool ret = DefensiveStrlenWithMinMax("hello", 1, 3, &out_len);
  EXPECT_FALSE(ret);
  EXPECT_EQ(out_len, 3u); // max we scanned
}

TEST(DefensiveStrlenWithMinMax, StringExceedsMaxNoOutLen) {
  bool ret = DefensiveStrlenWithMinMax("hello", 1, 3, nullptr);
  EXPECT_FALSE(ret);
}

// — Boundary: empty string ————————————————————————————————————————

TEST(DefensiveStrlenWithMinMax, EmptyStringMinZero) {
  size_t out_len = 42;
  // "" has length 0, min_sz = 0 → valid
  bool ret = DefensiveStrlenWithMinMax("", 0, 10, &out_len);
  EXPECT_TRUE(ret);
  EXPECT_EQ(out_len, 0u);
}

TEST(DefensiveStrlenWithMinMax, EmptyStringMinOne) {
  size_t out_len = 42;
  // "" has length 0, min_sz = 1 → too short
  bool ret = DefensiveStrlenWithMinMax("", 1, 10, &out_len);
  EXPECT_FALSE(ret);
  EXPECT_EQ(out_len, 0u);
}

// — Boundary: single character ————————————————————————————————————

TEST(DefensiveStrlenWithMinMax, SingleChar) {
  size_t out_len = 0;
  bool ret = DefensiveStrlenWithMinMax("X", 1, 10, &out_len);
  EXPECT_TRUE(ret);
  EXPECT_EQ(out_len, 1u);
}

// — NUL truncation: string with embedded NUL ——————————————————————

TEST(DefensiveStrlenWithMinMax, EmbeddedNulStopsEarly) {
  // "ab\0cd" — strlen would be 2 from C's perspective
  const char s[] = {'a', 'b', '\0', 'c', 'd', '\0'};
  size_t out_len = 0;
  bool ret = DefensiveStrlenWithMinMax(s, 1, 10, &out_len);
  EXPECT_TRUE(ret);
  EXPECT_EQ(out_len, 2u);
}
