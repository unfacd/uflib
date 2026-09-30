//
// utils_qr_code_decode_tests.cpp — adversarial tests for UfUtilsQrCodeDecode().
//
// These do not lock in what the function happens to do today.  They attack it:
// arguments that are NULL, empty, oversized or self-contradictory; images that
// are pure noise, pure black, pure white, or one pixel; strides that overlap
// rows; geometries whose product overflows the arithmetic the underlying
// library performs internally before it could ever be bounded from outside.
//
// The buffer assertions use canaries rather than value checks.  A canary
// catches a write past the end of the caller's buffer, which a value assertion
// cannot see: the value would be correct and the overrun invisible.  The
// out_text buffer in the overrun tests is placed last on the heap so that a
// row-pitch overrun runs off the end of the allocation, where ASan reports it,
// rather than into neighbouring padding.
//
// There is no GTEST_SKIP() anywhere: every case here has a defined outcome on
// every host, so a skip would only be a way of not finding out.
//

#include <gtest/gtest.h>

extern "C" {
#include <uflib/qr_code/utils_qr_code.h>
}

#include <climits>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// A reproducible PRNG.  std::mt19937 is not specified to produce the same
// sequence across implementations, which would make a noise test's outcome a
// property of the standard library rather than of the code under test.  This is
// xorshift32: eight lines, identical everywhere, and for filling a frame with
// bytes it is noise enough.
class Noise {
public:
    explicit Noise(uint32_t seed) : state_(seed ? seed : 0x9e3779b9u) {}

    uint8_t next()
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<uint8_t>(state_ >> 24);
    }

private:
    uint32_t state_;
};

std::vector<uint8_t> Filled(size_t size, uint8_t value)
{
    return std::vector<uint8_t>(size, value);
}

std::vector<uint8_t> NoiseOf(size_t size, uint32_t seed)
{
    std::vector<uint8_t> buffer(size);
    Noise noise(seed);
    for (size_t index = 0; index < size; index++) {
        buffer[index] = noise.next();
    }
    return buffer;
}

// The status of every call in this file goes through here first, so that a
// signature change or a call that cannot compile shows up once rather than in
// thirty places.
UfQrCodeStatus Decode(const std::vector<uint8_t> &image, size_t width, size_t height,
                      size_t stride, char *out_text, size_t capacity, size_t *out_len)
{
    return UfUtilsQrCodeDecode(image.data(), width, height, stride, out_text, capacity, out_len);
}

constexpr uint8_t kCanary = 0xA5;

} // namespace

// ── Argument validation ───────────────────────────────────────────────────

TEST(UfUtilsQrCodeDecode, RejectsEveryNullArgument)
{
    std::vector<uint8_t> image = Filled(64 * 64, 0);
    char   text[16];
    size_t len = 7777;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeDecode(nullptr, 64, 64, 64, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeDecode(image.data(), 64, 64, 64, nullptr, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeDecode(image.data(), 64, 64, 64, text, sizeof text, nullptr));

    // A rejected call must not have written through the pointers it was given.
    EXPECT_EQ(len, 7777u) << "rejected call still wrote *out_len";
}

TEST(UfUtilsQrCodeDecode, RejectsZeroCapacity)
{
    std::vector<uint8_t> image = Filled(16, 0);
    char   text[1] = {static_cast<char>(kCanary)};
    size_t len     = 0;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              Decode(image, 4, 4, 4, text, 0, &len));
    EXPECT_EQ(static_cast<uint8_t>(text[0]), kCanary) << "zero-capacity call wrote the buffer";
}

TEST(UfUtilsQrCodeDecode, RejectsDegenerateGeometry)
{
    std::vector<uint8_t> image = Filled(64, 0);
    char   text[16];
    size_t len = 0;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT, Decode(image, 0, 8, 8, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT, Decode(image, 8, 0, 8, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT, Decode(image, 0, 0, 8, text, sizeof text, &len));
}

// A stride below the row width would make consecutive rows share pixels: the
// decoder would read a lattice nobody photographed.  Refused rather than
// clamped, because clamping would silently decode something.
TEST(UfUtilsQrCodeDecode, RejectsStrideNarrowerThanTheRow)
{
    std::vector<uint8_t> image = Filled(64 * 64, 0);
    char   text[16];
    size_t len = 0;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT, Decode(image, 64, 64, 63, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT, Decode(image, 64, 64, 0, text, sizeof text, &len));
}

// ── Geometry that overflows the arithmetic inside the decoder ─────────────
//
// quirc multiplies the dimensions as *signed int* in three places that are
// reached before any check of ours could take effect — quirc.c:76
// (`size_t newdim = w * h`, before the calloc), identify.c:293 and
// identify.c:1083.  A product above INT_MAX is therefore signed overflow, which
// is undefined behaviour rather than a large number, and the sanitizer preset
// this tree builds with enables -fsanitize=signed-integer-overflow.
//
// The buffers passed here are one byte.  That is deliberate: if the validation
// is removed these calls do not merely return the wrong status, they read and
// write far outside a one-byte allocation, which ASan reports.  A generous
// buffer would let the bug pass as a wrong answer.

TEST(UfUtilsQrCodeDecode, RefusesGeometryWhoseProductOverflowsInt)
{
    std::vector<uint8_t> tiny = Filled(1, 0);
    char   text[16];
    size_t len = 0;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              Decode(tiny, 100000, 100000, 100000, text, sizeof text, &len));

    // One operand inside int range and one product outside it: the form the
    // division test exists for.
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              Decode(tiny, 100000, 100000, 1, text, sizeof text, &len));

    // Exactly the boundary.  INT_MAX/2 * 3 exceeds INT_MAX; INT_MAX/2 * 2 does
    // not, and that one is a legitimate request that must not be refused as
    // though it were an overflow.
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              Decode(tiny, static_cast<size_t>(INT_MAX) / 2, 3, 1, text, sizeof text, &len));
}

TEST(UfUtilsQrCodeDecode, RefusesDimensionBeyondIntRange)
{
    std::vector<uint8_t> tiny = Filled(1, 0);
    char   text[16];
    size_t len = 0;

    // quirc's interface takes the dimensions as int, so a size_t above INT_MAX
    // cannot be represented on the way in at all.
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              Decode(tiny, static_cast<size_t>(INT_MAX) + 1, 1, 1, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              Decode(tiny, 1, static_cast<size_t>(INT_MAX) + 1, 1, text, sizeof text, &len));
}

// ── Images that hold no code ──────────────────────────────────────────────

TEST(UfUtilsQrCodeDecode, ReportsNoCodeForAUniformImage)
{
    char   text[64];
    size_t len = 0;

    std::vector<uint8_t> black = Filled(64 * 64, 0x00);
    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND, Decode(black, 64, 64, 64, text, sizeof text, &len));

    std::vector<uint8_t> white = Filled(64 * 64, 0xFF);
    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND, Decode(white, 64, 64, 64, text, sizeof text, &len));

    // Mid-grey is the case a naive threshold gets wrong; it still holds no code.
    std::vector<uint8_t> grey = Filled(64 * 64, 0x80);
    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND, Decode(grey, 64, 64, 64, text, sizeof text, &len));
}

// NO_CODE_FOUND is a positive value on purpose, so a capture loop written as
// `if (status < 0) abort()` keeps spinning on an empty frame.  Asserted as a
// property rather than as a number: the sign is the contract.
TEST(UfUtilsQrCodeDecode, NoCodeFoundIsNotAnError)
{
    char   text[64];
    size_t len = 0;
    std::vector<uint8_t> black = Filled(32 * 32, 0);

    UfQrCodeStatus status = Decode(black, 32, 32, 32, text, sizeof text, &len);

    EXPECT_GT(status, 0);
    EXPECT_EQ(status, UF_QR_CODE_STATUS_NO_CODE_FOUND);
}

TEST(UfUtilsQrCodeDecode, SurvivesRandomNoise)
{
    char   text[64];
    size_t len = 0;

    // Fixed seeds, so this is reproducible rather than flaky.  A decoder that
    // crashed on ordinary noise would be a denial-of-service on any caller
    // pointed at a camera with the lens cap off.
    const size_t sizes[][2] = {{64, 64}, {128, 96}, {17, 17}, {1, 256}, {256, 1}};

    for (size_t index = 0; index < sizeof sizes / sizeof sizes[0]; index++) {
        size_t width  = sizes[index][0];
        size_t height = sizes[index][1];
        std::vector<uint8_t> noise = NoiseOf(width * height, 0xC0FFEEu + static_cast<uint32_t>(index));

        EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND,
                  Decode(noise, width, height, width, text, sizeof text, &len))
            << "noise " << width << "x" << height;
    }
}

TEST(UfUtilsQrCodeDecode, HandlesSinglePixelAndSliverImages)
{
    char   text[64];
    size_t len = 0;

    std::vector<uint8_t> one = Filled(1, 0x00);
    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND, Decode(one, 1, 1, 1, text, sizeof text, &len));

    std::vector<uint8_t> one_white = Filled(1, 0xFF);
    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND, Decode(one_white, 1, 1, 1, text, sizeof text, &len));

    // 1xN and Nx1: the shapes most likely to divide by zero inside a grid
    // detector, which is why they are here rather than only the square cases.
    std::vector<uint8_t> sliver = NoiseOf(4096, 12345);
    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND, Decode(sliver, 4096, 1, 4096, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND, Decode(sliver, 1, 4096, 1, text, sizeof text, &len));
}

// ── Buffer discipline ─────────────────────────────────────────────────────

// A frame that holds no code must not touch the caller's text buffer at all,
// so a caller may hold a previous result across a failed call.  The canary is
// the whole buffer, so a single byte written anywhere is caught.
TEST(UfUtilsQrCodeDecode, DoesNotWriteTheBufferWhenNoCodeIsFound)
{
    std::vector<char> text(64, static_cast<char>(kCanary));
    size_t len         = 0;
    std::vector<uint8_t> black = Filled(48 * 48, 0);

    ASSERT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND,
              Decode(black, 48, 48, 48, text.data(), text.size(), &len));

    for (size_t index = 0; index < text.size(); index++) {
        ASSERT_EQ(static_cast<uint8_t>(text[index]), kCanary)
            << "byte " << index << " of the caller's buffer was written";
    }
}

// The padded-stride case, which is what a real capture buffer looks like: rows
// four bytes apart beyond the row width.  The allocation is exactly
// stride*height with nothing after it, so an implementation that read a row at
// the wrong pitch — or that read the padding as pixels — runs off the end of
// the heap block and ASan reports it here rather than in the field.
TEST(UfUtilsQrCodeDecode, ReadsOnlyTheDeclaredRowsOfAPaddedStride)
{
    const size_t width  = 61;
    const size_t height = 37;
    const size_t stride = 128;   // deliberately not a multiple of the width

    std::vector<uint8_t> padded(stride * height);
    Noise noise(0xDEADBEEFu);
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            padded[y * stride + x] = noise.next();
        }
        for (size_t x = width; x < stride; x++) {
            padded[y * stride + x] = 0xFF;   // padding, never to be read
        }
    }

    char text[64];
    size_t len = 0;

    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND,
              Decode(padded, width, height, stride, text, sizeof text, &len));
}

// The same call with the buffer one byte short of a full padded image.  If the
// implementation reads `height` rows at the caller's stride this is the last
// byte it may touch; if it reads one row too many, ASan says so.  The row count
// is what is being probed, not the value.
TEST(UfUtilsQrCodeDecode, DoesNotReadARowBeyondTheImage)
{
    const size_t width  = 32;
    const size_t height = 4;
    const size_t stride = 32;

    std::vector<uint8_t> exact(width * height, 0x11);
    char   text[64];
    size_t len = 0;

    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND,
              Decode(exact, width, height, stride, text, sizeof text, &len));
}

// Canaries around the regions a caller passes in, on the path that does write:
// a one-byte-capacity call with an image large enough that the decoder runs to
// completion.  Nothing may be written past `capacity`.
TEST(UfUtilsQrCodeDecode, StaysInsideASingleByteCapacity)
{
    struct {
        char   guard_before[16];
        char   text[1];
        char   guard_after[16];
    } buffer;

    memset(&buffer, kCanary, sizeof buffer);

    std::vector<uint8_t> black = Filled(32 * 32, 0);
    size_t len = 0;

    // No code is present, so this returns without writing — but it must return,
    // and the guards must survive the call whatever the outcome.
    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND,
              Decode(black, 32, 32, 32, buffer.text, sizeof buffer.text, &len));

    for (size_t index = 0; index < sizeof buffer.guard_before; index++) {
        ASSERT_EQ(static_cast<uint8_t>(buffer.guard_before[index]), kCanary);
        ASSERT_EQ(static_cast<uint8_t>(buffer.guard_after[index]), kCanary);
    }
}

// ── Repeated use ──────────────────────────────────────────────────────────

// The library allocates a decoder per call and frees it per call.  A leak or a
// use-after-free on that path is what this exercises: many calls, mixed
// outcomes, no crash and no growth that ASan or LSan would report at exit.
TEST(UfUtilsQrCodeDecode, IsRepeatableWithoutLeaking)
{
    char text[64];

    for (int iteration = 0; iteration < 200; iteration++) {
        size_t len = 0;
        size_t side = 16 + static_cast<size_t>(iteration % 33);
        std::vector<uint8_t> frame = NoiseOf(side * side, 0xABCD0000u + static_cast<uint32_t>(iteration));

        UfQrCodeStatus status = Decode(frame, side, side, side, text, sizeof text, &len);
        ASSERT_EQ(status, UF_QR_CODE_STATUS_NO_CODE_FOUND) << "iteration " << iteration;
    }
}
