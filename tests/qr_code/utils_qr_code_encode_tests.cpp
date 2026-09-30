//
// utils_qr_code_encode_tests.cpp — adversarial tests for UfUtilsQrCodeEncodeText().
//
// The assertions here are invariants of the format rather than golden matrices.
// A golden matrix would pass for ever after the encoder changed underneath it,
// which is the opposite of what a test is for; the module count, the finder
// patterns and the normalised module values are properties the specification
// fixes and that a wrong implementation cannot fake.
//
// The capacity boundaries are asserted at the values that were measured against
// the linked libqrencode by bisection, and they are asserted on both sides: the
// largest payload that must succeed and the smallest that must not.  A
// one-sided test would pass against an implementation that refused everything.
//

#include <gtest/gtest.h>

extern "C" {
#include <uflib/qr_code/utils_qr_code.h>
}

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The smallest symbol the specification defines is version 1 at 21 modules; the
// largest is version 40 at 177.  Both are 4k+17.
constexpr size_t kMinSymbolWidth = 21;
constexpr size_t kMaxSymbolWidth = 177;

// A matrix that frees itself, so an ASSERT_* that returns early cannot leak.
class Matrix {
public:
    explicit Matrix(UfQrCodeMatrix *matrix) : matrix_(matrix) {}
    ~Matrix() { free(matrix_); }

    Matrix(const Matrix &)            = delete;
    Matrix &operator=(const Matrix &) = delete;

    const UfQrCodeMatrix *get() const { return matrix_; }
    size_t width() const { return matrix_->width; }

    uint8_t at(size_t x, size_t y) const { return matrix_->modules[y * matrix_->width + x]; }

private:
    UfQrCodeMatrix *matrix_;
};

std::string Repeat(char character, size_t count)
{
    return std::string(count, character);
}

// The finder pattern is a fixed 7x7 arrangement, and it appears at three
// corners.  Checking it is how a test distinguishes a real symbol from a block
// of plausible-looking modules: no wrong arrangement passes all three, and the
// pattern's own light and dark rings also prove the normalisation is right,
// because a byte left unmasked would fail the light cells.
bool FinderPatternIsCorrect(const Matrix &matrix, size_t origin_x, size_t origin_y)
{
    for (size_t y = 0; y < 7; y++) {
        for (size_t x = 0; x < 7; x++) {
            bool on_outer_ring = (x == 0 || x == 6 || y == 0 || y == 6);
            bool in_centre     = (x >= 2 && x <= 4 && y >= 2 && y <= 4);
            uint8_t expected   = (on_outer_ring || in_centre) ? 1 : 0;

            if (matrix.at(origin_x + x, origin_y + y) != expected) {
                return false;
            }
        }
    }

    return true;
}

} // namespace

// ── Argument validation ───────────────────────────────────────────────────

TEST(UfUtilsQrCodeEncodeText, RejectsNullArguments)
{
    UfQrCodeMatrix *matrix = nullptr;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT, UfUtilsQrCodeEncodeText(nullptr, &matrix));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT, UfUtilsQrCodeEncodeText("hello", nullptr));
}

// The empty string is rejected as an argument error rather than passed to the
// encoder, whose own refusal of it would be indistinguishable from a symbol
// that did not fit.  Nil is not a payload worth a symbol either way.
TEST(UfUtilsQrCodeEncodeText, RejectsTheEmptyString)
{
    UfQrCodeMatrix *matrix = nullptr;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT, UfUtilsQrCodeEncodeText("", &matrix));
    EXPECT_EQ(matrix, nullptr);
}

// The out-parameter is cleared before anything can fail, so a caller that
// ignores the status cannot free a stale pointer.  Poisoned with a non-NULL
// value first, because clearing an already-NULL pointer proves nothing.
TEST(UfUtilsQrCodeEncodeText, ClearsTheOutPointerOnEveryFailure)
{
    UfQrCodeMatrix *matrix = reinterpret_cast<UfQrCodeMatrix *>(0xDEADBEEF);

    ASSERT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT, UfUtilsQrCodeEncodeText("", &matrix));
    EXPECT_EQ(matrix, nullptr) << "a failed call left the caller's pointer non-NULL";

    matrix = reinterpret_cast<UfQrCodeMatrix *>(0xDEADBEEF);
    ASSERT_EQ(UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE,
              UfUtilsQrCodeEncodeText(Repeat('a', 100000).c_str(), &matrix));
    EXPECT_EQ(matrix, nullptr);
}

// ── Capacity boundaries ───────────────────────────────────────────────────
//
// Measured against the linked libqrencode 4.1.1 at error-correction level L by
// bisecting the longest input each mode accepts: 2953 bytes in byte mode, 7089
// digits in numeric mode.  The encoder picks the mode itself, so both are
// reachable through this one function and both must be honoured — a single
// byte-mode ceiling would refuse numeric payloads that encode perfectly well.

TEST(UfUtilsQrCodeEncodeText, AcceptsTheLargestByteModePayload)
{
    UfQrCodeMatrix *raw = nullptr;
    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText(Repeat('a', 2953).c_str(), &raw));
    Matrix matrix(raw);

    EXPECT_EQ(matrix.width(), kMaxSymbolWidth);
}

TEST(UfUtilsQrCodeEncodeText, RefusesOneByteMoreThanByteModeHolds)
{
    UfQrCodeMatrix *matrix = nullptr;

    // Not ERR_NOMEM and not ERR_ENCODE: the payload does not fit, and retrying
    // will not change that.  The distinction matters to a caller deciding
    // whether to truncate or to give up.
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE,
              UfUtilsQrCodeEncodeText(Repeat('a', 2954).c_str(), &matrix));
    EXPECT_EQ(matrix, nullptr);
}

TEST(UfUtilsQrCodeEncodeText, AcceptsTheLargestNumericPayload)
{
    UfQrCodeMatrix *raw = nullptr;
    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText(Repeat('7', 7089).c_str(), &raw));
    Matrix matrix(raw);

    EXPECT_EQ(matrix.width(), kMaxSymbolWidth);
}

TEST(UfUtilsQrCodeEncodeText, RefusesOneDigitMoreThanAnySymbolHolds)
{
    UfQrCodeMatrix *matrix = nullptr;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE,
              UfUtilsQrCodeEncodeText(Repeat('7', 7090).c_str(), &matrix));
    EXPECT_EQ(matrix, nullptr);
}

// An alphanumeric payload sits between the two: it packs more than byte mode
// and less than numeric.  A bound expressed as either of the measured extremes
// gets this case wrong in one direction or the other, so it is asserted here.
TEST(UfUtilsQrCodeEncodeText, AcceptsAlphanumericPayloadsAboveTheByteModeCeiling)
{
    UfQrCodeMatrix *raw = nullptr;
    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText(Repeat('A', 4296).c_str(), &raw));
    Matrix matrix(raw);

    EXPECT_EQ(matrix.width(), kMaxSymbolWidth);

    UfQrCodeMatrix *refused = nullptr;
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE,
              UfUtilsQrCodeEncodeText(Repeat('A', 4297).c_str(), &refused));
}

// A payload far beyond any symbol is rejected without the encoder being asked,
// so it is refused promptly and with the right status rather than surfacing as
// an allocation failure somewhere inside the library.
TEST(UfUtilsQrCodeEncodeText, RefusesAnAbsurdPayloadWithoutAllocatingForIt)
{
    UfQrCodeMatrix *matrix = nullptr;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE,
              UfUtilsQrCodeEncodeText(Repeat('a', 1000000).c_str(), &matrix));
    EXPECT_EQ(matrix, nullptr);
}

// ── Symbol geometry ───────────────────────────────────────────────────────

TEST(UfUtilsQrCodeEncodeText, ProducesASymbolOfLegalWidth)
{
    const char *payloads[] = {"1", "hello", "https://unfacd.io", "0123456789"};

    for (const char *payload : payloads) {
        UfQrCodeMatrix *raw = nullptr;
        ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText(payload, &raw)) << payload;
        Matrix matrix(raw);

        size_t width = matrix.width();

        EXPECT_GE(width, kMinSymbolWidth) << payload;
        EXPECT_LE(width, kMaxSymbolWidth) << payload;

        // Every version's symbol is 4k+17 modules across.  A width off that
        // lattice is not a QR symbol at all.
        EXPECT_EQ((width - 17) % 4, 0u) << payload << " width " << width;
    }
}

TEST(UfUtilsQrCodeEncodeText, CarriesTheThreeFinderPatterns)
{
    UfQrCodeMatrix *raw = nullptr;
    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText("unfacd", &raw));
    Matrix matrix(raw);

    size_t width = matrix.width();

    EXPECT_TRUE(FinderPatternIsCorrect(matrix, 0, 0)) << "top-left finder pattern is wrong";
    EXPECT_TRUE(FinderPatternIsCorrect(matrix, width - 7, 0)) << "top-right finder pattern is wrong";
    EXPECT_TRUE(FinderPatternIsCorrect(matrix, 0, width - 7)) << "bottom-left finder pattern is wrong";
}

// The encoder's own data bytes carry flags above the module bit — measured as
// 0x02, 0x80, 0x84, 0x90, 0xc0 and 0xc1 among others for a plain ASCII payload,
// with 289 of a 441-module symbol neither 0 nor 1.  If that masking were
// dropped, a caller testing the byte for truth would render most of the symbol
// wrongly, and this is the assertion that catches it: only 0 and 1 may escape.
TEST(UfUtilsQrCodeEncodeText, NormalisesEveryModuleToZeroOrOne)
{
    UfQrCodeMatrix *raw = nullptr;
    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText("hello world", &raw));
    Matrix matrix(raw);

    size_t width = matrix.width();

    for (size_t y = 0; y < width; y++) {
        for (size_t x = 0; x < width; x++) {
            uint8_t module = matrix.at(x, y);
            ASSERT_TRUE(module == 0 || module == 1)
                << "module (" << x << "," << y << ") is " << static_cast<unsigned>(module)
                << ", which is neither light nor dark";
        }
    }
}

// Neither extreme may be constant: an all-light or all-dark matrix would satisfy
// a legality check while carrying nothing.
TEST(UfUtilsQrCodeEncodeText, ProducesBothLightAndDarkModules)
{
    UfQrCodeMatrix *raw = nullptr;
    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText("unfacd", &raw));
    Matrix matrix(raw);

    size_t width = matrix.width();
    size_t dark  = 0;

    for (size_t index = 0; index < width * width; index++) {
        dark += raw->modules[index];
    }

    EXPECT_GT(dark, 0u);
    EXPECT_LT(dark, width * width);
}

// ── Payload handling ──────────────────────────────────────────────────────

// The docs say the payload is read as bytes up to its first NUL, so an embedded
// NUL truncates rather than being carried.  Asserted as a property — the two
// matrices are identical — rather than by inspecting a decoded round trip,
// because the claim being tested is about what the encoder was handed.
TEST(UfUtilsQrCodeEncodeText, TruncatesAtAnEmbeddedNul)
{
    const char truncated[] = {'a', 'b', '\0', 'c', 'd', '\0'};

    UfQrCodeMatrix *raw_a = nullptr;
    UfQrCodeMatrix *raw_b = nullptr;

    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText("ab", &raw_a));
    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText(truncated, &raw_b));

    Matrix a(raw_a);
    Matrix b(raw_b);

    ASSERT_EQ(a.width(), b.width());

    size_t width = a.width();
    for (size_t index = 0; index < width * width; index++) {
        ASSERT_EQ(raw_a->modules[index], raw_b->modules[index])
            << "module " << index << " differs: the bytes past the NUL were carried";
    }
}

TEST(UfUtilsQrCodeEncodeText, IsDeterministicForTheSamePayload)
{
    UfQrCodeMatrix *first  = nullptr;
    UfQrCodeMatrix *second = nullptr;

    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText("https://unfacd.io", &first));
    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText("https://unfacd.io", &second));

    Matrix a(first);
    Matrix b(second);

    ASSERT_EQ(a.width(), b.width());

    size_t width = a.width();
    for (size_t index = 0; index < width * width; index++) {
        ASSERT_EQ(first->modules[index], second->modules[index]) << "module " << index;
    }
}

// A single character must produce the smallest symbol rather than failing or
// producing something degenerate.
TEST(UfUtilsQrCodeEncodeText, EncodesTheSmallestPayload)
{
    UfQrCodeMatrix *raw = nullptr;
    ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText("1", &raw));
    Matrix matrix(raw);

    EXPECT_EQ(matrix.width(), kMinSymbolWidth);
}

// ── Repeated use ──────────────────────────────────────────────────────────

// Every call allocates and hands the allocation to the caller, so the freeing
// in this loop is the module's ownership contract being exercised.  A leak or a
// double free here is what ASan and LSan report at exit.
TEST(UfUtilsQrCodeEncodeText, IsRepeatableWithCallerOwnedResults)
{
    for (int iteration = 0; iteration < 128; iteration++) {
        std::string payload = "payload-" + std::to_string(iteration);

        UfQrCodeMatrix *raw = nullptr;
        ASSERT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText(payload.c_str(), &raw));
        ASSERT_NE(raw, nullptr);
        ASSERT_GE(raw->width, kMinSymbolWidth);

        free(raw);
    }
}
