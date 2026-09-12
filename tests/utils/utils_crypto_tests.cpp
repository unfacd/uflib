// Adversarial test suite for src/utils_crypto.c
//
// The tests below are deliberately adversarial: alongside ordinary
// correctness checks they probe boundary values, malicious inputs, and
// contract violations.  Where the implementation deviates from its own
// documented/implied contract, the corresponding test is labelled with a
// `// DEFECT:` comment and asserts the *correct* behaviour (so it fails,
// exposing the flaw) rather than locking in the buggy behaviour.

#include "gtest/gtest.h"

#include <cstdint>
#include <cstddef>
#include <cstdlib>   // rand / srand / RAND_MAX
#include <climits>   // ULONG_MAX
#include <cctype>    // isxdigit
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "uflib/utils_crypto.h"
}

#ifndef SHA256_DIGEST_LENGTH
# define SHA256_DIGEST_LENGTH 32 // OpenSSL SHA-256 output size in bytes
#endif

// ── helpers ────────────────────────────────────────────────────────────────

// Encode `n` bytes at `p` as lowercase hex into a std::string.
static std::string HexEncode(const void *p, size_t n)
{
  static const char digits[] = "0123456789abcdef";
  const unsigned char *b = static_cast<const unsigned char *>(p);
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    out.push_back(digits[b[i] >> 4]);
    out.push_back(digits[b[i] & 0x0F]);
  }
  return out;
}

// ── strcmp_constant_time ───────────────────────────────────────────────────

TEST(StrcmpConstantTime, EqualBuffersReturnZero)
{
  const unsigned char a[] = {0xDE, 0xAD, 0xBE, 0xEF};
  const unsigned char b[] = {0xDE, 0xAD, 0xBE, 0xEF};
  EXPECT_EQ(strcmp_constant_time(a, b, sizeof(a)), 0);
}

TEST(StrcmpConstantTime, DifferingBuffersReturnNonzero)
{
  const unsigned char a[] = {0xDE, 0xAD, 0xBE, 0xEF};
  const unsigned char b[] = {0xDE, 0xAD, 0xBE, 0xEE};
  EXPECT_NE(strcmp_constant_time(a, b, sizeof(a)), 0);
}

TEST(StrcmpConstantTime, SingleByteDifferenceDetected)
{
  const unsigned char a[] = {0x00};
  const unsigned char b[] = {0x01};
  EXPECT_NE(strcmp_constant_time(a, b, 1), 0);
}

TEST(StrcmpConstantTime, ZeroSizeReturnsZero)
{
  // Zero-length comparison never enters the loop; the result is 0 ("equal").
  // (strcmp_constant_time is annotated nonnull, so we pass valid pointers.)
  const unsigned char a[] = {0x42};
  EXPECT_EQ(strcmp_constant_time(a, a, 0), 0);
}

TEST(StrcmpConstantTime, ExhaustiveAllByteValuesEqual)
{
  unsigned char a[256], b[256];
  for (int i = 0; i < 256; i++) {
    a[i] = static_cast<unsigned char>(i);
    b[i] = static_cast<unsigned char>(i);
  }
  EXPECT_EQ(strcmp_constant_time(a, b, 256), 0);
}

// ── strcmp_time_constant2 ──────────────────────────────────────────────────

TEST(StrcmpTimeConstant2, EqualStringsReturnZero)
{
  char a[] = "constant-time-compare";
  char b[] = "constant-time-compare";
  EXPECT_EQ(strcmp_time_constant2(a, b), 0);
}

TEST(StrcmpTimeConstant2, DifferingStringsReturnNonzero)
{
  char a[] = "constant-time-compare";
  char b[] = "constant-time-COMPARE";
  EXPECT_NE(strcmp_time_constant2(a, b), 0);
}

TEST(StrcmpTimeConstant2, DifferentLengthsReturnNonzero)
{
  char a[] = "short";
  char b[] = "short-but-longer";
  EXPECT_NE(strcmp_time_constant2(a, b), 0);
}

TEST(StrcmpTimeConstant2, EmbeddedNulStopsComparison)
{
  char a[] = {'a', 'b', '\0', 'X', 'Y', '\0'};
  char b[] = {'a', 'b', '\0', 'P', 'Q', '\0'};
  // strlen sees both as "ab" — the trailing bytes are ignored.
  EXPECT_EQ(strcmp_time_constant2(a, b), 0);
}

TEST(StrcmpTimeConstant2, BoundaryAtMbufEqual)
{
  // MBUF == 256. A string of exactly 256 chars is still comparable.
  std::vector<char> a(257, 'x'), b(257, 'x');
  a[256] = '\0';
  b[256] = '\0';
  EXPECT_EQ(strcmp_time_constant2(a.data(), b.data()), 0);
}

TEST(StrcmpTimeConstant2, OverMbufIdenticalStringsReportDifferent)
{
  // Documented behaviour: strings longer than MBUF are rejected (return 1),
  // so two *identical* 257-char strings compare as "different".
  std::vector<char> a(258, 'y'), b(258, 'y');
  a[257] = '\0';
  b[257] = '\0';
  EXPECT_NE(strcmp_time_constant2(a.data(), b.data()), 0);
}

// ── memcmp_constant_time ───────────────────────────────────────────────────

TEST(MemcmpConstantTime, EqualBuffersReturnZero)
{
  const char a[] = "memcmp-constant-time";
  const char b[] = "memcmp-constant-time";
  EXPECT_EQ(memcmp_constant_time(a, b, sizeof(a)), 0);
}

TEST(MemcmpConstantTime, DifferingBuffersReturnNonzero)
{
  const char a[] = "memcmp-constant-time";
  const char b[] = "memcmp-constant-TIME";
  EXPECT_NE(memcmp_constant_time(a, b, sizeof(a)), 0);
}

TEST(MemcmpConstantTime, ZeroSizeReturnsZero)
{
  EXPECT_EQ(memcmp_constant_time(nullptr, nullptr, 0), 0);
}

// ── memcpy_constant_time ───────────────────────────────────────────────────

// NOTE: despite the "memcpy" name, this function's signature takes no
// destination to write into and its body is byte-for-byte identical to
// memcmp_constant_time — it performs a comparison, not a copy.  The tests
// below assert the *comparison* behaviour it actually implements; the
// misleading name is flagged in the accompanying findings.

TEST(MemcpyConstantTime, BehavesAsCompareEqual)
{
  const char a[] = "sneaky-name";
  const char b[] = "sneaky-name";
  EXPECT_EQ(memcpy_constant_time(a, b, sizeof(a)), 0);
}

TEST(MemcpyConstantTime, BehavesAsCompareDiffer)
{
  const char a[] = "sneaky-name";
  const char b[] = "sneaky-NOME";
  EXPECT_NE(memcpy_constant_time(a, b, sizeof(a)), 0);
}

TEST(MemcpyConstantTime, ZeroSizeReturnsZero)
{
  EXPECT_EQ(memcpy_constant_time(nullptr, nullptr, 0), 0);
}

// ── memcpy_constant_time2 (real constant-time copy) ────────────────────────

TEST(MemcpyConstantTime2, CopiesBytesCorrectly)
{
  const unsigned char src[] = {0x00, 0x01, 0x7f, 0x80, 0xff, 0xde, 0xad, 0xbe, 0xef};
  unsigned char dest[sizeof(src)];
  memset(dest, 0xAA, sizeof(dest));

  void *ret = memcpy_constant_time2(dest, src, sizeof(src));

  EXPECT_EQ(ret, static_cast<void *>(dest));
  EXPECT_EQ(memcmp(dest, src, sizeof(src)), 0);
}

TEST(MemcpyConstantTime2, ZeroLengthIsNoop)
{
  unsigned char dest[] = {0x42};
  const unsigned char src[] = {0x99};
  void *ret = memcpy_constant_time2(dest, src, 0);
  EXPECT_EQ(ret, static_cast<void *>(dest));
  EXPECT_EQ(dest[0], 0x42); // untouched
}

TEST(MemcpyConstantTime2, DoesNotOverwriteBeyondLength)
{
  // Sentinel bytes before/after dest must be untouched.
  const unsigned char src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  unsigned char buf[10];
  memset(buf, 0xEE, sizeof(buf));
  memcpy_constant_time2(buf + 1, src, sizeof(src));
  EXPECT_EQ(buf[0], 0xEE);
  EXPECT_EQ(buf[9], 0xEE);
  EXPECT_EQ(memcmp(buf + 1, src, sizeof(src)), 0);
}

TEST(MemcpyConstantTime2, CopiesBinaryDataIncludingNul)
{
  // Must copy embedded NUL bytes (not stop at the first NUL).
  const unsigned char src[] = {'a', '\0', 'b', '\0', 0xff};
  unsigned char dest[sizeof(src)] = {0};
  memcpy_constant_time2(dest, src, sizeof(src));
  EXPECT_EQ(memcmp(dest, src, sizeof(src)), 0);
}

TEST(MemcpyConstantTime2, LargeCopy)
{
  std::vector<unsigned char> src(4096), dest(4096);
  for (size_t i = 0; i < src.size(); i++) {
    src[i] = static_cast<unsigned char>(i * 7 + 1);
  }
  memcpy_constant_time2(dest.data(), src.data(), src.size());
  EXPECT_EQ(memcmp(dest.data(), src.data(), src.size()), 0);
}

// ── ComputeSHA1 ────────────────────────────────────────────────────────────

TEST(ComputeSha1, HexOfAbc)
{
  char out[SHA_DIGEST_LENGTH * 2 + 1] = {0};
  const unsigned char *in = reinterpret_cast<const unsigned char *>("abc");
  ComputeSHA1(in, 3, out, sizeof(out), 0u);
  EXPECT_STREQ(out, "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(ComputeSha1, HexOfEmpty)
{
  char out[SHA_DIGEST_LENGTH * 2 + 1] = {0};
  ComputeSHA1(reinterpret_cast<const unsigned char *>(""), 0, out, sizeof(out), 0u);
  EXPECT_STREQ(out, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(ComputeSha1, HexOfQuickBrownFox)
{
  char out[SHA_DIGEST_LENGTH * 2 + 1] = {0};
  const char *msg = "The quick brown fox jumps over the lazy dog";
  ComputeSHA1(reinterpret_cast<const unsigned char *>(msg), strlen(msg), out,
              sizeof(out), 0u);
  EXPECT_STREQ(out, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST(ComputeSha1, Base64OfAbc)
{
  // base64(SHA1("abc")) == base64 of the 20-byte digest.
  char out[32] = {0};
  const unsigned char *in = reinterpret_cast<const unsigned char *>("abc");
  int rc = ComputeSHA1(in, 3, out, sizeof(out), 1u);
  EXPECT_GT(rc, 0);
  EXPECT_STREQ(out, "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
}

TEST(ComputeSha1, HexReturnValueIsDigestLengthNotPointerSize)
{
  // DEFECT: the hex path returns `sizeof(output)` (== sizeof(char*) == 8),
  // not the produced string length.  The correct contract for a successful
  // hex encode is SHA_DIGEST_LENGTH * 2 characters.
  char out[SHA_DIGEST_LENGTH * 2 + 1] = {0};
  const unsigned char *in = reinterpret_cast<const unsigned char *>("abc");
  int rc = ComputeSHA1(in, 3, out, sizeof(out), 0u);
  EXPECT_EQ(rc, static_cast<int>(SHA_DIGEST_LENGTH * 2));
}

// ── ComputeHmacSha256 ──────────────────────────────────────────────────────

TEST(ComputeHmacSha256, Rfc4231Case1)
{
  // RFC 4231 Test Case 1: key = 0x0b repeated 20 times, data = "Hi There".
  unsigned char key[20];
  memset(key, 0x0b, sizeof(key));
  unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
  const unsigned char *data = reinterpret_cast<const unsigned char *>("Hi There");
  ComputeHmacSha256(data, 8, key, sizeof(key), digest);
  EXPECT_EQ(HexEncode(digest, SHA256_DIGEST_LENGTH),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(ComputeHmacSha256, Rfc4231Case2)
{
  const unsigned char *key = reinterpret_cast<const unsigned char *>("Jefe");
  const unsigned char *data =
      reinterpret_cast<const unsigned char *>("what do ya want for nothing?");
  unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
  ComputeHmacSha256(data, 28, key, 4, digest);
  EXPECT_EQ(HexEncode(digest, SHA256_DIGEST_LENGTH),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(ComputeHmacSha256, Rfc4231LongKey)
{
  // Key longer than the 64-byte block: the function must first hash it.
  std::vector<unsigned char> key(131, 0xaa);
  const unsigned char *data = reinterpret_cast<const unsigned char *>(
      "Test Using Larger Than Block-Size Key - Hash Key First");
  unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
  ComputeHmacSha256(data, 54, key.data(), static_cast<int>(key.size()), digest);
  EXPECT_EQ(HexEncode(digest, SHA256_DIGEST_LENGTH),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// Regression: text longer than the old fixed 1024-byte buffer (961 > 960) must
// hash correctly now that bufferIn is sized 64 + text_len (malloc'd).
TEST(ComputeHmacSha256, TextLongerThan960Works)
{
  std::vector<unsigned char> text(961, 'A');
  std::vector<unsigned char> key(32, 0x00);
  unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
  ComputeHmacSha256(text.data(), static_cast<int>(text.size()), key.data(),
                    static_cast<int>(key.size()), digest);
  EXPECT_EQ(HexEncode(digest, SHA256_DIGEST_LENGTH),
            "abf6e7c4eb89f02fd9eb3c699f62ad7485c56d999bbec94221c76d966b50fb6c");
}

// ── GenerateSalt ───────────────────────────────────────────────────────────

TEST(GenerateSalt, ZeroTerminatedProducesTwoHexPerByte)
{
  const unsigned length = 16;
  unsigned char *s = GenerateSalt(length, true);
  ASSERT_NE(s, nullptr);
  // 16 random bytes → 32 hex chars, NUL-terminated.
  EXPECT_EQ(strlen(reinterpret_cast<char *>(s)), 2u * length);
  EXPECT_EQ(s[2 * length], '\0');
  free(s);
}

TEST(GenerateSalt, NonZeroTerminatedShouldStillBeLengthBytes)
{
  // DEFECT: the ternary allocating the salt buffer is inverted
  //   `zero_terminated ? (salt_length += length) : (salt_length += length+1)`.
  // With zero_terminated == false it therefore allocates (and hex-encodes)
  // length+1 bytes — the final un-randomised calloc zero byte becomes a
  // spurious trailing "00".  The correct contract: the salt is `length`
  // random bytes, so the hex encoding is exactly 2*length characters.
  const unsigned length = 16;
  unsigned char *s = GenerateSalt(length, false);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(strlen(reinterpret_cast<char *>(s)), 2u * length);
  free(s);
}

// ── GenerateSecureRandom ───────────────────────────────────────────────────

TEST(GenerateSecureRandom, ReturnsZeroAndFills)
{
  uint8_t a[32] = {0};
  uint8_t b[32] = {0};
  EXPECT_EQ(GenerateSecureRandom(a, sizeof(a)), 0);
  EXPECT_EQ(GenerateSecureRandom(b, sizeof(b)), 0);
  // Two independent draws must not be identical (2^-256 collision chance).
  EXPECT_NE(memcmp(a, b, sizeof(a)), 0);
}

TEST(GenerateSecureRandom, ZeroLengthIsNoop)
{
  uint8_t buf[1] = {0};
  EXPECT_EQ(GenerateSecureRandom(buf, 0), 0);
  EXPECT_EQ(buf[0], 0); // untouched
}

// ── GenerateSecureRandomHexed ──────────────────────────────────────────────

TEST(GenerateSecureRandomHexed, AllocatesHexString)
{
  char *s = GenerateSecureRandomHexed(nullptr, 16);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(strlen(s), 16u);   // buffer_sz == 16 hex chars
  EXPECT_EQ(s[16], '\0');
  for (int i = 0; i < 16; i++) {
    EXPECT_TRUE(isxdigit(static_cast<unsigned char>(s[i])));
  }
  free(s);
}

TEST(GenerateSecureRandomHexed, UsesProvidedBuffer)
{
  char buf[17] = {0};
  char *ret = GenerateSecureRandomHexed(buf, 16);
  EXPECT_EQ(ret, buf);          // returned the caller's buffer
  EXPECT_EQ(strlen(buf), 16u);
  EXPECT_EQ(buf[16], '\0');     // relies on the caller pre-zeroing the buffer
  for (int i = 0; i < 16; i++) {
    EXPECT_TRUE(isxdigit(static_cast<unsigned char>(buf[i])));
  }
}

TEST(GenerateSecureRandomHexed, LargerEvenSize)
{
  char *s = GenerateSecureRandomHexed(nullptr, 64);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(strlen(s), 64u);
  EXPECT_EQ(s[64], '\0');
  free(s);
}

// Regression: odd buffer_sz now yields exactly buffer_sz hex chars (random
// byte count rounds up), and the result is NUL-terminated.
TEST(GenerateSecureRandomHexed, OddSizeProducesExactChars)
{
  char *s = GenerateSecureRandomHexed(nullptr, 5);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(strlen(s), 5u);
  EXPECT_EQ(s[5], '\0');
  for (int i = 0; i < 5; i++) {
    EXPECT_TRUE(isxdigit(static_cast<unsigned char>(s[i])));
  }
  free(s);
}

TEST(GenerateSecureRandomHexed, ProvidedBufferIsNulTerminated)
{
  // The provided buffer is deliberately NOT zeroed — the function must now
  // write its own NUL terminator.
  char buf[9];
  memset(buf, 'X', sizeof(buf));
  char *ret = GenerateSecureRandomHexed(buf, 8);
  EXPECT_EQ(ret, buf);
  EXPECT_EQ(buf[8], '\0');
  EXPECT_EQ(strlen(buf), 8u);
  for (int i = 0; i < 8; i++) {
    EXPECT_TRUE(isxdigit(static_cast<unsigned char>(buf[i])));
  }
}

// ── hex_print ──────────────────────────────────────────────────────────────

TEST(HexPrint, AllocatesUppercaseHex)
{
  const unsigned char in[] = {0x00, 0x01, 0x0f, 0x10, 0xab, 0xff};
  unsigned char *out = hex_print(in, sizeof(in), nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(reinterpret_cast<char *>(out), "00010F10ABFF");
  free(out);
}

TEST(HexPrint, UsesProvidedBuffer)
{
  const unsigned char in[] = {0xde, 0xad};
  unsigned char buf[5] = {0};
  unsigned char *ret = hex_print(in, 2, buf);
  EXPECT_EQ(ret, buf);
  EXPECT_EQ(memcmp(buf, "DEAD", 4), 0);
}

TEST(HexPrint, ZeroLengthYieldsEmpty)
{
  const unsigned char in[] = {0x42};
  unsigned char *out = hex_print(in, 0, nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out[0], '\0');
  free(out);
}

// ── GenerateRandomNumber ───────────────────────────────────────────────────

TEST(GenerateRandomNumber, DeterministicGivenSeed)
{
  srand(12345);
  unsigned long a = GenerateRandomNumber();
  srand(12345);
  unsigned long b = GenerateRandomNumber();
  EXPECT_EQ(a, b);
}

TEST(GenerateRandomNumber, VariesAcrossSeeds)
{
  srand(1);
  unsigned long a = GenerateRandomNumber();
  srand(2);
  unsigned long b = GenerateRandomNumber();
  EXPECT_NE(a, b);
}

TEST(GenerateRandomNumber, FitsUnsignedLong)
{
  srand(7);
  for (int i = 0; i < 1000; i++) {
    unsigned long v = GenerateRandomNumber();
    EXPECT_LE(v, ULONG_MAX); // trivially true; guards against UB via wraparound
  }
}

// ── GenerateRandomNumberWithUpper ──────────────────────────────────────────

TEST(GenerateRandomNumberWithUpper, MaxZeroYieldsZero)
{
  srand(42);
  for (int i = 0; i < 100; i++) {
    EXPECT_EQ(GenerateRandomNumberWithUpper(0), 0u);
  }
}

TEST(GenerateRandomNumberWithUpper, ResultWithinBounds)
{
  srand(99);
  for (long max : {1L, 2L, 10L, 100L, 1000000L}) {
    for (int i = 0; i < 2000; i++) {
      unsigned long v = GenerateRandomNumberWithUpper(max);
      EXPECT_LE(v, static_cast<unsigned long>(max));
    }
  }
}

TEST(GenerateRandomNumberWithUpper, RandMaxBoundary)
{
  srand(5);
  for (int i = 0; i < 2000; i++) {
    unsigned long v = GenerateRandomNumberWithUpper(RAND_MAX);
    EXPECT_LE(v, static_cast<unsigned long>(RAND_MAX));
  }
}

// Regression: invalid max (outside [0, RAND_MAX]) must not divide by zero —
// it returns 0.
TEST(GenerateRandomNumberWithUpper, NegativeMaxReturnsZero)
{
  srand(1);
  EXPECT_EQ(GenerateRandomNumberWithUpper(-1), 0u);
  EXPECT_EQ(GenerateRandomNumberWithUpper(-1000000), 0u);
  EXPECT_EQ(GenerateRandomNumberWithUpper(LONG_MIN), 0u);
}

TEST(GenerateRandomNumberWithUpper, AboveRandMaxReturnsZero)
{
  srand(1);
  EXPECT_EQ(GenerateRandomNumberWithUpper(static_cast<long>(RAND_MAX) + 1), 0u);
}

// ── GenerateRandomNumberBounded ────────────────────────────────────────────

TEST(GenerateRandomNumberBounded, ResultWithinHalfOpenRange)
{
  srand(2024);
  for (int i = 0; i < 5000; i++) {
    long v = GenerateRandomNumberBounded(10, 20);
    EXPECT_GE(v, 10);
    EXPECT_LT(v, 20);
  }
}

TEST(GenerateRandomNumberBounded, NegativeBounds)
{
  srand(77);
  for (int i = 0; i < 5000; i++) {
    long v = GenerateRandomNumberBounded(-100, -50);
    EXPECT_GE(v, -100);
    EXPECT_LT(v, -50);
  }
}

// Regression: empty/inverted range (min >= max) must not divide by zero —
// it returns min.
TEST(GenerateRandomNumberBounded, EmptyRangeReturnsMin)
{
  srand(1);
  EXPECT_EQ(GenerateRandomNumberBounded(5, 5), 5);
  EXPECT_EQ(GenerateRandomNumberBounded(10, 5), 10);
  EXPECT_EQ(GenerateRandomNumberBounded(-5, -5), -5);
}

// ── GetNextExponentialBackoffValue ─────────────────────────────────────────

TEST(GetNextExponentialBackoffValue, RecurrenceZeroYieldsMin)
{
  EXPECT_EQ(GetNextExponentialBackoffValue(0, 10, 1000), 10);
}

TEST(GetNextExponentialBackoffValue, MonotonicNonDecreasing)
{
  int prev = GetNextExponentialBackoffValue(0, 0, 1000);
  for (int r = 1; r <= 20; r++) {
    int cur = GetNextExponentialBackoffValue(r, 0, 1000);
    EXPECT_GE(cur, prev);
    prev = cur;
  }
}

TEST(GetNextExponentialBackoffValue, CappedAtMax)
{
  // Large recurrence drives pow() far past max; result must clamp to max.
  EXPECT_EQ(GetNextExponentialBackoffValue(1000000, 0, 500), 500);
  EXPECT_EQ(GetNextExponentialBackoffValue(5000, 100, 100), 100);
}

TEST(GetNextExponentialBackoffValue, NeverBelowMin)
{
  for (int r = 0; r <= 50; r++) {
    EXPECT_GE(GetNextExponentialBackoffValue(r, 42, 1000), 42);
  }
}

// ── Encrypt / Decrypt with signalling key ──────────────────────────────────

// A fixed 52-byte signalling key (32-byte AES-256 cipher key + 20-byte HMAC
// key), base64-encoded.  Derived from bytes 0x00..0x33.
static const char *kKeyB64 =
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMw==";

// Round-trip an exact multiple of AES blocks (regression for the
// EncryptWithSignallingKey stack-overflow fix at src/utils_crypto.c:523).
TEST(EncryptDecryptSignallingKey, RoundTripBlockAligned)
{
  std::vector<unsigned char> plain(32, 0); // exactly two AES blocks
  for (size_t i = 0; i < plain.size(); i++) plain[i] = static_cast<unsigned char>(i * 3 + 1);

  EncryptedMessage *enc = EncryptWithSignallingKey(
      plain.data(), plain.size(), reinterpret_cast<unsigned char *>(const_cast<char *>(kKeyB64)), true);
  ASSERT_NE(enc, nullptr);
  ASSERT_NE(enc->final_message_b64, nullptr);

  DecryptedMessage *dec = DecryptWithSignallingKey(
      enc->final_message_b64, strlen(reinterpret_cast<char *>(enc->final_message_b64)),
      reinterpret_cast<unsigned char *>(const_cast<char *>(kKeyB64)), true);
  ASSERT_NE(dec, nullptr);

  EXPECT_GE(dec->size, plain.size());
  EXPECT_EQ(memcmp(dec->msg.msg_clear, plain.data(), plain.size()), 0);

  EncryptedMessageDestruct(enc, true);
  DecryptedMessageDestruct(dec, true);
}

TEST(EncryptDecryptSignallingKey, RoundTripNonBlockAligned)
{
  // 20 bytes = one full block + a 4-byte partial block.  Adversarial: the
  // padded block-length handling must recover the exact plaintext.
  const char *plain = "partial block test!";
  size_t textlen = strlen(plain);

  EncryptedMessage *enc = EncryptWithSignallingKey(
      reinterpret_cast<const unsigned char *>(plain), textlen,
      reinterpret_cast<unsigned char *>(const_cast<char *>(kKeyB64)), true);
  ASSERT_NE(enc, nullptr);
  ASSERT_NE(enc->final_message_b64, nullptr);

  DecryptedMessage *dec = DecryptWithSignallingKey(
      enc->final_message_b64, strlen(reinterpret_cast<char *>(enc->final_message_b64)),
      reinterpret_cast<unsigned char *>(const_cast<char *>(kKeyB64)), true);
  ASSERT_NE(dec, nullptr);

  EXPECT_GE(dec->size, textlen);
  EXPECT_EQ(memcmp(dec->msg.msg_clear, plain, textlen), 0);

  EncryptedMessageDestruct(enc, true);
  DecryptedMessageDestruct(dec, true);
}

// Corrupt the ciphertext and assert the truncated HMAC rejects it.
TEST(EncryptDecryptSignallingKey, TamperedCiphertextRejected)
{
  const char *plain = "tamper-proof";
  size_t textlen = strlen(plain);

  EncryptedMessage *enc = EncryptWithSignallingKey(
      reinterpret_cast<const unsigned char *>(plain), textlen,
      reinterpret_cast<unsigned char *>(const_cast<char *>(kKeyB64)), true);
  ASSERT_NE(enc, nullptr);
  ASSERT_NE(enc->final_message_b64, nullptr);

  // Flip one base64 character in the middle (valid base64 alphabet both
  // before and after), corrupting the decoded ciphertext → the 10-byte MAC
  // must mismatch and decryption must be refused.
  std::string tampered(reinterpret_cast<char *>(enc->final_message_b64));
  size_t mid = tampered.size() / 2;
  tampered[mid] = (tampered[mid] == 'A') ? 'B' : 'A';

  DecryptedMessage *dec = DecryptWithSignallingKey(
      reinterpret_cast<const unsigned char *>(tampered.c_str()), tampered.size(),
      reinterpret_cast<unsigned char *>(const_cast<char *>(kKeyB64)), true);
  EXPECT_EQ(dec, nullptr);

  EncryptedMessageDestruct(enc, true);
}

TEST(EncryptDecryptSignallingKey, RawKeyPathShouldWork)
{
  // DEFECT: when flag_b64encoded_key == false the function sets
  //   `b64decoded_key = key;` but leaves rc_len == 0, so the subsequent
  //   `rc_len < CIPHER_KEY_SIZE + MAC_KEY_SIZE` check always rejects the raw
  //   key path.  The raw-key branch is therefore dead — it can never produce
  //   a valid EncryptedMessage.  The correct contract (implied by the flag)
  //   is that a raw 52-byte key works identically to its base64 encoding.
  unsigned char raw_key[52] = {0};
  for (int i = 0; i < 52; i++) raw_key[i] = static_cast<unsigned char>(i);

  const char *plain = "raw-key";
  EncryptedMessage *enc = EncryptWithSignallingKey(
      reinterpret_cast<const unsigned char *>(plain), strlen(plain), raw_key, false);
  EXPECT_NE(enc, nullptr);
  if (enc) {
    EncryptedMessageDestruct(enc, true);
  }
}

// ── Message destructors ────────────────────────────────────────────────────

TEST(MessageDestruct, EmptyEncryptedMessageSelfDestruct)
{
  EncryptedMessage *enc = static_cast<EncryptedMessage *>(calloc(1, sizeof(EncryptedMessage)));
  ASSERT_NE(enc, nullptr);
  // All fields NULL → destructor must be a safe no-op for the frees, then
  // free the struct itself (flag_selfdestruct == true).
  EncryptedMessageDestruct(enc, true);
}

TEST(MessageDestruct, EmptyEncryptedMessageKeepStruct)
{
  EncryptedMessage *enc = static_cast<EncryptedMessage *>(calloc(1, sizeof(EncryptedMessage)));
  ASSERT_NE(enc, nullptr);
  EncryptedMessageDestruct(enc, false);
  // Struct still alive and zeroed; a second destruct with selfdestruct=true
  // must not double-free (fields are NULL after the first memset).
  EncryptedMessageDestruct(enc, true);
}

TEST(MessageDestruct, EmptyDecryptedMessageSelfDestruct)
{
  DecryptedMessage *dec = static_cast<DecryptedMessage *>(calloc(1, sizeof(DecryptedMessage)));
  ASSERT_NE(dec, nullptr);
  DecryptedMessageDestruct(dec, true);
}
