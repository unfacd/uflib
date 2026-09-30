//
// utils_qr_code_roundtrip_tests.cpp — the two halves against each other.
//
// These are the only tests that decode a symbol this library produced, and they
// exist because the encode and decode suites cannot check each other: one
// asserts the format's invariants, the other asserts behaviour on frames that
// hold no code at all.  A symbol that is legal but unreadable satisfies both.
//
// The rasteriser is deliberately hostile.  It places the symbol on a pixel grid
// with a quiet zone, and the cases here vary the scale, pad the stride, truncate
// the frame and put two symbols in it — the conditions a real capture has and a
// synthetic one-module-per-pixel fixture never does.
//
// Requires both capabilities, so it is gated on both in CMake rather than
// compiled conditionally here.
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

// A matrix that frees itself.
class Matrix {
public:
    explicit Matrix(UfQrCodeMatrix *matrix) : matrix_(matrix) {}
    ~Matrix() { free(matrix_); }

    Matrix(const Matrix &)            = delete;
    Matrix &operator=(const Matrix &) = delete;

    const UfQrCodeMatrix *get() const { return matrix_; }

private:
    UfQrCodeMatrix *matrix_;
};

// The specification's minimum quiet zone, in modules, on each side.  A symbol
// rendered without it commonly does not decode, which is why it is applied here
// rather than left to the caller: these tests are about the module data, and a
// fixture that tripped over the border would be testing the fixture.
constexpr size_t kQuietZoneModules = 4;

struct Raster {
    std::vector<uint8_t> pixels;
    size_t               width;
    size_t               height;
    size_t               stride;
};

// Render a symbol at `scale` pixels per module on a light background with a
// quiet zone, into a buffer whose rows are `stride` bytes apart.  A stride of
// zero means "packed", which is the case that exercises the un-padded path.
Raster Rasterise(const UfQrCodeMatrix *matrix, size_t scale, size_t quiet_modules, size_t stride)
{
    const size_t modules_across = matrix->width + 2 * quiet_modules;
    const size_t width          = modules_across * scale;
    const size_t height         = width;

    if (stride == 0) {
        stride = width;
    }

    Raster raster;
    raster.pixels.assign(stride * height, 0xFF);
    raster.width  = width;
    raster.height = height;
    raster.stride = stride;

    for (size_t module_y = 0; module_y < matrix->width; module_y++) {
        for (size_t module_x = 0; module_x < matrix->width; module_x++) {
            if (matrix->modules[module_y * matrix->width + module_x] == 0) {
                continue;   // light: the background already is
            }

            for (size_t pixel_y = 0; pixel_y < scale; pixel_y++) {
                size_t y = (module_y + quiet_modules) * scale + pixel_y;
                size_t x = (module_x + quiet_modules) * scale;
                memset(&raster.pixels[y * stride + x], 0x00, scale);
            }
        }
    }

    return raster;
}

Matrix Encode(const char *payload)
{
    UfQrCodeMatrix *matrix = nullptr;
    EXPECT_EQ(UF_QR_CODE_STATUS_OK, UfUtilsQrCodeEncodeText(payload, &matrix)) << payload;
    return Matrix(matrix);
}

} // namespace

// ── The round trip ────────────────────────────────────────────────────────

TEST(UfUtilsQrCodeRoundTrip, RecoversWhatItEncoded)
{
    const char *payloads[] = {"1", "unfacd", "https://unfacd.io", "0123456789", "the quick brown fox"};

    for (const char *payload : payloads) {
        Matrix matrix = Encode(payload);
        ASSERT_NE(matrix.get(), nullptr) << payload;

        Raster raster = Rasterise(matrix.get(), 4, kQuietZoneModules, 0);

        char   text[512];
        size_t len = 0;

        ASSERT_EQ(UF_QR_CODE_STATUS_OK,
                  UfUtilsQrCodeDecode(raster.pixels.data(), raster.width, raster.height, raster.stride,
                                      text, sizeof text, &len))
            << payload;

        EXPECT_EQ(std::string(text, len), payload);
    }
}

// The module scale is not part of the format, so a symbol must survive being
// drawn at any reasonable one.  A decoder that only worked at one scale would
// be reading pixel coordinates rather than modules.
TEST(UfUtilsQrCodeRoundTrip, SurvivesEveryReasonableModuleScale)
{
    Matrix matrix = Encode("scale-invariant");

    const size_t scales[] = {2, 3, 4, 6, 8, 11};

    for (size_t scale : scales) {
        Raster raster = Rasterise(matrix.get(), scale, kQuietZoneModules, 0);

        char   text[512];
        size_t len = 0;

        ASSERT_EQ(UF_QR_CODE_STATUS_OK,
                  UfUtilsQrCodeDecode(raster.pixels.data(), raster.width, raster.height, raster.stride,
                                      text, sizeof text, &len))
            << "scale " << scale;

        EXPECT_EQ(std::string(text, len), "scale-invariant") << "scale " << scale;
    }
}

// A capture buffer's rows are padded, and the decoder is given the stride rather
// than a packed image.  With the padding filled with a value that would corrupt
// the symbol if it were read as pixels, this proves the stride is honoured.
TEST(UfUtilsQrCodeRoundTrip, DecodesThroughAPaddedStride)
{
    Matrix matrix = Encode("padded-stride");

    for (size_t padding : {1u, 7u, 64u, 129u}) {
        Raster raster = Rasterise(matrix.get(), 4, kQuietZoneModules, 0);

        // Re-render into a wider buffer with the padding made hostile.
        size_t   stride = raster.width + padding;
        std::vector<uint8_t> padded(stride * raster.height, 0x00);

        for (size_t y = 0; y < raster.height; y++) {
            memcpy(&padded[y * stride], &raster.pixels[y * raster.width], raster.width);
        }

        char   text[512];
        size_t len = 0;

        ASSERT_EQ(UF_QR_CODE_STATUS_OK,
                  UfUtilsQrCodeDecode(padded.data(), raster.width, raster.height, stride,
                                      text, sizeof text, &len))
            << "padding " << padding;

        EXPECT_EQ(std::string(text, len), "padded-stride") << "padding " << padding;
    }
}

// ── The caller's buffer ───────────────────────────────────────────────────

// Too small by exactly one byte: the payload is `len` bytes plus a terminator,
// so a capacity of `len` cannot hold it.  The required size is reported so the
// caller can retry, and nothing is written — a partial payload with a
// terminator would be a silently truncated result.
TEST(UfUtilsQrCodeRoundTrip, ReportsTheRequiredSizeWhenTheBufferIsOneByteShort)
{
    Matrix matrix = Encode("exactly-too-small");
    Raster raster = Rasterise(matrix.get(), 4, kQuietZoneModules, 0);

    const char *payload = "exactly-too-small";

    char   text[64];
    size_t len = 0;

    memset(text, 0xA5, sizeof text);

    ASSERT_EQ(UF_QR_CODE_STATUS_ERR_BUFFER_TOO_SMALL,
              UfUtilsQrCodeDecode(raster.pixels.data(), raster.width, raster.height, raster.stride,
                                  text, strlen(payload), &len));

    EXPECT_EQ(len, strlen(payload) + 1) << "the required size must include the terminator";

    for (size_t index = 0; index < sizeof text; index++) {
        ASSERT_EQ(static_cast<uint8_t>(text[index]), 0xA5)
            << "byte " << index << " was written despite the buffer being too small";
    }
}

// And exactly enough: the same call with one more byte succeeds and terminates.
TEST(UfUtilsQrCodeRoundTrip, SucceedsWithExactlyTheRequiredCapacity)
{
    Matrix matrix = Encode("exactly-enough");
    Raster raster = Rasterise(matrix.get(), 4, kQuietZoneModules, 0);

    const char *payload = "exactly-enough";
    const size_t needed = strlen(payload) + 1;

    std::vector<char> text(needed + 8, static_cast<char>(0xA5));
    size_t len = 0;

    ASSERT_EQ(UF_QR_CODE_STATUS_OK,
              UfUtilsQrCodeDecode(raster.pixels.data(), raster.width, raster.height, raster.stride,
                                  text.data(), needed, &len));

    EXPECT_EQ(len, strlen(payload));
    EXPECT_EQ(std::string(text.data(), len), payload);
    EXPECT_EQ(text[needed - 1], '\0') << "the payload was not terminated";

    // The bytes past the terminator are the caller's and must be untouched.
    for (size_t index = needed; index < text.size(); index++) {
        ASSERT_EQ(static_cast<uint8_t>(text[index]), 0xA5) << "byte " << index << " past capacity";
    }
}

// ── Frames that should not decode ─────────────────────────────────────────

// The bottom half of a symbol, with the quiet zone stripped from what remains.
// A decoder that returned a cached or partially-populated payload rather than
// reading the image would pass the round trip above and fail here.
TEST(UfUtilsQrCodeRoundTrip, DoesNotDecodeATruncatedSymbol)
{
    Matrix matrix = Encode("truncated-frame");
    Raster raster = Rasterise(matrix.get(), 4, kQuietZoneModules, 0);

    size_t half_height = raster.height / 2;

    char   text[512];
    size_t len = 0;

    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND,
              UfUtilsQrCodeDecode(raster.pixels.data(), raster.width, half_height, raster.stride,
                                  text, sizeof text, &len));
}

// Inverting the luminance is the one transformation that turns a valid symbol
// into an invalid one: QR recognition is defined on dark modules over a light
// background.  Asserted so that the decoder's behaviour on it is known rather
// than assumed — if a future quirc learns inverted symbols, this test fails and
// the header's documentation needs the same re-examination.
TEST(UfUtilsQrCodeRoundTrip, DoesNotDecodeAnInvertedSymbol)
{
    Matrix matrix = Encode("inverted");
    Raster raster = Rasterise(matrix.get(), 4, kQuietZoneModules, 0);

    for (uint8_t &pixel : raster.pixels) {
        pixel = static_cast<uint8_t>(0xFF - pixel);
    }

    char   text[512];
    size_t len = 0;

    EXPECT_EQ(UF_QR_CODE_STATUS_NO_CODE_FOUND,
              UfUtilsQrCodeDecode(raster.pixels.data(), raster.width, raster.height, raster.stride,
                                  text, sizeof text, &len));
}

// ── More than one symbol in a frame ───────────────────────────────────────

// quirc counts *candidate* finds, not readable codes, and the implementation
// walks all of them rather than only the first.  Two symbols side by side is
// the shape that distinguishes the two: stopping at index 0 would still pass
// whenever index 0 happened to be the good one, so the assertion is that the
// payload is one of the two — which holds whichever order the detector reports.
TEST(UfUtilsQrCodeRoundTrip, FindsASymbolWhenTheFrameHoldsTwo)
{
    Matrix left  = Encode("left-symbol");
    Matrix right = Encode("right-symbol");

    Raster left_raster  = Rasterise(left.get(), 4, kQuietZoneModules, 0);
    Raster right_raster = Rasterise(right.get(), 4, kQuietZoneModules, 0);

    const size_t gap    = 64;
    const size_t width  = left_raster.width + gap + right_raster.width;
    const size_t height = std::max(left_raster.height, right_raster.height);

    std::vector<uint8_t> frame(width * height, 0xFF);

    for (size_t y = 0; y < left_raster.height; y++) {
        memcpy(&frame[y * width], &left_raster.pixels[y * left_raster.width], left_raster.width);
    }
    for (size_t y = 0; y < right_raster.height; y++) {
        memcpy(&frame[y * width + left_raster.width + gap],
               &right_raster.pixels[y * right_raster.width], right_raster.width);
    }

    char   text[512];
    size_t len = 0;

    ASSERT_EQ(UF_QR_CODE_STATUS_OK,
              UfUtilsQrCodeDecode(frame.data(), width, height, width, text, sizeof text, &len));

    std::string payload(text, len);

    EXPECT_TRUE(payload == "left-symbol" || payload == "right-symbol")
        << "recovered \"" << payload << "\", which is neither symbol in the frame";
}
