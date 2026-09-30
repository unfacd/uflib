/**
 * @file ufconfig_transform_tests.cpp
 * @brief The transform catalogue and the validators.
 *
 * Reached through ufconfig_internals_shim.h rather than by including the
 * module's private header here: that header needs C17 <stdatomic.h>, which does
 * not survive a C++ translation unit.  See the shim's own header for the
 * reasoning.
 *
 * A transform that claims to be reversible is expected to be.  The shim
 * distinguishes "the round trip changed the data" from "an operation failed",
 * because the two mean different things and only one of them is a defect.
 */

#include <gtest/gtest.h>

#include "ufconfig_internals_shim.h"

#include <string>
#include <vector>

namespace {

constexpr int kRoundTripOk = 0;
constexpr int kOperationFailed = 1;
constexpr int kRoundTripChangedData = 2;

void ExpectLossless(const std::string &op, const std::string &input) {
    SCOPED_TRACE(op + " on " + std::to_string(input.size()) + " bytes");
    // The shim reports 2 for a lossy transform and 1 for a failure, so a
    // lossless transform must report exactly 0.
    EXPECT_EQ(ufconfigShimTransformRoundTrip(op.c_str(), input.data(), input.size()), kRoundTripOk);
}

void ExpectLossy(const std::string &op, const std::string &input) {
    SCOPED_TRACE(op);
    int verdict = ufconfigShimTransformRoundTrip(op.c_str(), input.data(), input.size());
    EXPECT_TRUE(verdict == kRoundTripOk || verdict == kRoundTripChangedData)
        << "the operation itself failed (verdict " << verdict << ")";
}

}  // namespace

// ── Reversible codecs ────────────────────────────────────────────────────────

TEST(UfConfigTransform, HexRoundTrips) {
    ExpectLossless("hex", "abc");
    ExpectLossless("hex", "");
    ExpectLossless("hex", std::string("\x00\x01\xff\xfe", 4));
    ExpectLossless("hex", std::string(300, 'Z'));
}

TEST(UfConfigTransform, Base64AndItsUrlVariantRoundTrip) {
    ExpectLossless("base64", "hello");
    ExpectLossless("base64", "");
    ExpectLossless("base64", std::string("\x00\x01\xff\xfe", 4));
    ExpectLossless("base64url", "hello?");
    // The two alphabets differ exactly where base64 emits + and /, so a payload
    // that exercises both is the one worth carrying.
    ExpectLossless("base64url", std::string("\xfb\xff\xbf\xef", 4));
}

TEST(UfConfigTransform, Base32RoundTrips) {
    ExpectLossless("base32", "hello");
    ExpectLossless("base32", "");
    ExpectLossless("base32", std::string(64, 'Q'));
}

TEST(UfConfigTransform, CaseTransformsAreLossyAndSaySo) {
    ExpectLossy("toupper", "shadow");
    ExpectLossy("tolower", "SHADOW");
}

// ── Strict decode ────────────────────────────────────────────────────────────
//
// A decoder that accepts malformed input is worse than one that crashes: the
// corruption surfaces later, somewhere unrelated.

TEST(UfConfigTransform, HexDecoderRejectsOddLengthAndNonHexDigits) {
    EXPECT_EQ(ufconfigShimDecodeRejected("hex", "zz", 2), 1);
    EXPECT_EQ(ufconfigShimDecodeRejected("hex", "abc", 3), 1)
        << "an odd number of hex digits is not a whole number of bytes";
    EXPECT_EQ(ufconfigShimDecodeRejected("hex", "ab", 2), 0) << "valid hex must still decode";
}

TEST(UfConfigTransform, Base64DecoderRejectsGarbage) {
    EXPECT_EQ(ufconfigShimDecodeRejected("base64", "!!!!", 4), 1);
    EXPECT_EQ(ufconfigShimDecodeRejected("base64", "a", 1), 1)
        << "a single base64 character cannot encode a byte";
    EXPECT_EQ(ufconfigShimDecodeRejected("base64", "aGVsbG8=", 8), 0)
        << "valid base64 must still decode";
}

TEST(UfConfigTransform, Base32DecoderRejectsCharactersOutsideItsAlphabet) {
    // 0, 1, 8 and 9 are absent from the base32 alphabet precisely because they
    // are easy to confuse with O, I, B and g.
    EXPECT_EQ(ufconfigShimDecodeRejected("base32", "0189", 4), 1);
    EXPECT_EQ(ufconfigShimDecodeRejected("base32", "NBSWY3DP", 8), 0)
        << "valid base32 must still decode";
}

// ── Compression ──────────────────────────────────────────────────────────────

TEST(UfConfigTransform, CompressRoundTripsWhatItIsGiven) {
    ExpectLossless("compress", "");
    ExpectLossless("compress", "x");
    ExpectLossless("compress", std::string(400, 'A'));
}

// ── Validators ───────────────────────────────────────────────────────────────

TEST(UfConfigValidator, DateRejectsImpossibleDaysAndAcceptsLeapDays) {
    EXPECT_EQ(ufconfigShimValid("date", "2025-02-30"), 0) << "February has no 30th";
    EXPECT_EQ(ufconfigShimValid("date", "2024-02-29"), 1) << "2024 is a leap year";
    EXPECT_EQ(ufconfigShimValid("date", "2025-02-29"), 0) << "2025 is not a leap year";
    EXPECT_EQ(ufconfigShimValid("date", "2025-13-01"), 0) << "there is no month 13";
    EXPECT_EQ(ufconfigShimValid("date", "2025-00-10"), 0) << "there is no month 0";
}

TEST(UfConfigValidator, Ip4RejectsLeadingZeros) {
    // "1.2.3.04" is ambiguous: it is octal to some parsers and decimal to
    // others, so it must be refused rather than guessed at.
    EXPECT_EQ(ufconfigShimValid("ip4", "1.2.3.04"), 0);
    EXPECT_EQ(ufconfigShimValid("ip4", "127.0.0.1"), 1);
    EXPECT_EQ(ufconfigShimValid("ip4", "256.0.0.1"), 0);
    EXPECT_EQ(ufconfigShimValid("ip4", "1.2.3"), 0);
    EXPECT_EQ(ufconfigShimValid("ip4", "1.2.3.4.5"), 0);
}

TEST(UfConfigValidator, Ip6AcceptsOneCompactionOnly) {
    EXPECT_EQ(ufconfigShimValid("ip6", "1::2::3"), 0) << ":: may appear at most once";
    EXPECT_EQ(ufconfigShimValid("ip6", "::1"), 1);
    EXPECT_EQ(ufconfigShimValid("ip6", "::"), 1);
    EXPECT_EQ(ufconfigShimValid("ip6", "2001:db8::1"), 1);
    EXPECT_EQ(ufconfigShimValid("ip6", "12345::1"), 0) << "a group is at most four hex digits";
}

TEST(UfConfigValidator, EmailRequiresBothSides) {
    EXPECT_EQ(ufconfigShimValid("email", "@"), 0);
    EXPECT_EQ(ufconfigShimValid("email", "a@"), 0);
    EXPECT_EQ(ufconfigShimValid("email", "@b.com"), 0);
    EXPECT_EQ(ufconfigShimValid("email", "a@b.com"), 1);
}

TEST(UfConfigValidator, UrlRequiresASchemeAndAHost) {
    EXPECT_EQ(ufconfigShimValid("url", "/rel"), 0) << "a relative path is not a URL";
    EXPECT_EQ(ufconfigShimValid("url", "https://host/x"), 1);
    EXPECT_EQ(ufconfigShimValid("url", "https://"), 0);
}

TEST(UfConfigValidator, FqdnRejectsUnderscores) {
    EXPECT_EQ(ufconfigShimValid("fqdn", "bad_host"), 0)
        << "an underscore is legal in a DNS label but not in a hostname";
    EXPECT_EQ(ufconfigShimValid("fqdn", "db.ufsrv.unfacd.com"), 1);
    EXPECT_EQ(ufconfigShimValid("fqdn", "-leading.host"), 0);
    EXPECT_EQ(ufconfigShimValid("fqdn", "trailing-.host"), 0);
}

TEST(UfConfigValidator, FileSizeAcceptsBothSiAndBinarySuffixes) {
    EXPECT_EQ(ufconfigShimValid("filesize", "10MiB"), 1);
    EXPECT_EQ(ufconfigShimValid("filesize", "10MB"), 1);
    EXPECT_EQ(ufconfigShimValid("filesize", "10xx"), 0);
}

TEST(UfConfigValidator, UnknownValidatorIsRefusedRatherThanPassed) {
    EXPECT_EQ(ufconfigShimValid("no-such-validator", "anything"), 0);
}
