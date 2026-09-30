//
// utils_qr_code_camera_tests.cpp — adversarial tests for
// UfUtilsQrCodeCameraGenerate().
//
// The function opens a device, and everything interesting about it is what it
// does when the device is not one.  A camera is not needed to test any of that:
// the failure paths are reachable with /dev/null, a regular file, a path that
// does not exist, and a file with no read permission — and those are the paths
// a production bug would live on, because they are the ones nobody exercises by
// hand.
//
// Two of these are worth more than the rest:
//
//   * /dev/null and a regular file both open() successfully.  That they are
//     still rejected is what proves the setup checks VIDIOC_QUERYCAP and the
//     node's capabilities rather than merely that a descriptor came back.
//
//   * The descriptor-count test.  A teardown that closes only on the success
//     path leaks a descriptor per failed call, which is invisible in any single
//     assertion and fatal in a capture loop.  Counting /proc/self/fd across 64
//     failures is the only way to see it.
//
// No GTEST_SKIP(): on a host with no /dev/video0 the correct outcome is
// ERR_DEVICE, which is an assertion like any other.  Skipping by capability is
// a CMake decision — with the capability off these symbols do not exist, so a
// runtime skip could not even link.
//

#include <gtest/gtest.h>

extern "C" {
#include <uflib/qr_code/utils_qr_code.h>
}

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// A scratch file that removes itself, so a failing assertion cannot leave
// litter behind for the next run to trip over.
class ScratchFile {
public:
    explicit ScratchFile(const char *name, mode_t mode)
        : path_(std::string("/tmp/uflib_qr_code_") + name + "_" + std::to_string(getpid()))
    {
        int fd = open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
        if (fd >= 0) {
            ssize_t written = write(fd, "not a camera\n", 13);
            (void)written;
            close(fd);
        }
        if (chmod(path_.c_str(), mode) != 0) {
            // Reported by the test that depends on the mode, not here.
        }
    }

    ~ScratchFile() { unlink(path_.c_str()); }

    ScratchFile(const ScratchFile &)            = delete;
    ScratchFile &operator=(const ScratchFile &) = delete;

    const char *path() const { return path_.c_str(); }

private:
    std::string path_;
};

// The descriptors this process holds right now.  Read as a count rather than as
// a set: the call under test opens and closes its own, and any difference
// between two readings taken the same way is the call's doing.
long OpenDescriptorCount()
{
    DIR *directory = opendir("/proc/self/fd");
    if (directory == nullptr) {
        return -1;
    }

    long count = 0;

    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == nullptr) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        count++;
    }

    closedir(directory);

    return count;
}

struct CameraCall {
    UfQrCodeStatus status;
    int            errno_value;
    size_t         len;
    char           text[128];
};

// errno is only meaningful when ERR_DEVICE is returned — the header says so —
// so it is captured here unconditionally and only asserted where that holds.
CameraCall Capture(const char *device, int width, int height, unsigned timeout_ms)
{
    CameraCall result;

    result.len = 0;
    memset(result.text, 0, sizeof result.text);

    errno       = 0;
    result.status = UfUtilsQrCodeCameraGenerate(device, width, height, timeout_ms,
                                                result.text, sizeof result.text, &result.len);
    result.errno_value = errno;

    return result;
}

} // namespace

// ── Argument validation ───────────────────────────────────────────────────

TEST(UfUtilsQrCodeCameraGenerate, RejectsDeviceThatIsNullOrEmpty)
{
    char   text[64];
    size_t len = 0;

    // No silent fallback to /dev/video0: a mistyped path in a call site must
    // fail loudly rather than open whichever camera happens to be first.
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate(nullptr, 640, 480, 0, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("", 640, 480, 0, text, sizeof text, &len));
}

TEST(UfUtilsQrCodeCameraGenerate, RejectsEveryNullOutArgument)
{
    size_t len = 0;
    char   text[64];

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("/dev/video0", 640, 480, 0, nullptr, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("/dev/video0", 640, 480, 0, text, sizeof text, nullptr));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("/dev/video0", 640, 480, 0, text, 0, &len));
}

TEST(UfUtilsQrCodeCameraGenerate, RejectsNonPositiveGeometry)
{
    char   text[64];
    size_t len = 0;

    // The arguments are int here rather than size_t, so the negative half of
    // the range is reachable and a caller converting from a computed size can
    // land on it.
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("/dev/video0", 0, 480, 0, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("/dev/video0", 640, 0, 0, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("/dev/video0", -640, 480, 0, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("/dev/video0", 640, INT_MIN, 0, text, sizeof text, &len));
}

// The requested geometry reaches the same signed multiply inside quirc, so it
// is refused on the same terms as in the decode path.  The check runs before
// the device is touched, which the nonexistent path in this test makes
// observable: a request that overflowed would otherwise report ERR_DEVICE.
TEST(UfUtilsQrCodeCameraGenerate, RefusesGeometryWhoseProductOverflowsInt)
{
    char   text[64];
    size_t len = 0;

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("/dev/video0", 100000, 100000, 0, text, sizeof text, &len));
    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("/dev/video0", INT_MAX, 3, 0, text, sizeof text, &len));
}

// A rejected call must not have written the caller's buffer or length, so that
// a caller may hold a previous result across it.
TEST(UfUtilsQrCodeCameraGenerate, DoesNotWriteOutputsWhenItRefusesTheArguments)
{
    char   text[64];
    size_t len = 4242;

    memset(text, 0xA5, sizeof text);

    ASSERT_EQ(UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT,
              UfUtilsQrCodeCameraGenerate("", 640, 480, 0, text, sizeof text, &len));

    EXPECT_EQ(len, 4242u);

    for (size_t index = 0; index < sizeof text; index++) {
        ASSERT_EQ(static_cast<uint8_t>(text[index]), 0xA5) << "byte " << index;
    }
}

// ── Things that are not cameras ───────────────────────────────────────────

TEST(UfUtilsQrCodeCameraGenerate, ReportsDeviceErrorForAPathThatDoesNotExist)
{
    CameraCall call = Capture("/dev/video-does-not-exist-uflib", 640, 480, 0);

    EXPECT_EQ(call.status, UF_QR_CODE_STATUS_ERR_DEVICE);
    EXPECT_EQ(call.errno_value, ENOENT) << "errno should carry the open() failure verbatim";
    EXPECT_EQ(call.len, 0u);
}

// /dev/null opens successfully.  It is not a camera, and the only way to know
// that is to ask it — which is the point of the test.
TEST(UfUtilsQrCodeCameraGenerate, ReportsDeviceErrorForDevNull)
{
    CameraCall call = Capture("/dev/null", 640, 480, 0);

    EXPECT_EQ(call.status, UF_QR_CODE_STATUS_ERR_DEVICE);
    EXPECT_EQ(call.len, 0u);

#ifdef __linux__
    // VIDIOC_QUERYCAP on a non-video descriptor fails with ENOTTY, which is the
    // syscall's own errno and is surfaced unchanged.
    EXPECT_EQ(call.errno_value, ENOTTY);
#endif
}

// A regular file opens successfully too, and with the right permissions on a
// permissive umask it is readable and writable.  It must still be refused.
TEST(UfUtilsQrCodeCameraGenerate, ReportsDeviceErrorForARegularFile)
{
    ScratchFile file("regular_file", 0644);

    CameraCall call = Capture(file.path(), 640, 480, 0);

    EXPECT_EQ(call.status, UF_QR_CODE_STATUS_ERR_DEVICE);
    EXPECT_EQ(call.len, 0u);
}

// A file that cannot be opened at all: permission is refused by open() itself
// rather than by any later check, so this is the one case where the errno is
// EACCES.  Root ignores the mode, so the expectation accommodates a run as
// root rather than assuming the privilege level of the test host.
TEST(UfUtilsQrCodeCameraGenerate, ReportsDeviceErrorForAnUnreadablePath)
{
    ScratchFile file("unreadable", 0000);

    CameraCall call = Capture(file.path(), 640, 480, 0);

    EXPECT_EQ(call.status, UF_QR_CODE_STATUS_ERR_DEVICE);
    EXPECT_TRUE(call.errno_value == EACCES || call.errno_value == ENOTTY)
        << "unexpected errno " << call.errno_value << " (" << strerror(call.errno_value) << ")";
}

TEST(UfUtilsQrCodeCameraGenerate, ReportsDeviceErrorForADirectoryPath)
{
    CameraCall call = Capture("/tmp", 640, 480, 0);

    // open() of a directory with O_RDWR fails EISDIR; if a platform allowed it,
    // the capability check would refuse it instead.  Either way it is a device
    // error and never a success.
    EXPECT_EQ(call.status, UF_QR_CODE_STATUS_ERR_DEVICE);
    EXPECT_EQ(call.len, 0u);
}

// ── Descriptor discipline ─────────────────────────────────────────────────

// The check that catches a teardown written only on the success path.  A single
// failed call leaking one descriptor is invisible here and fatal in a loop that
// retries a camera every frame, so it is counted over enough repetitions that a
// leak of even one per call is unmistakable.
TEST(UfUtilsQrCodeCameraGenerate, DoesNotLeakDescriptorsAcrossRepeatedFailures)
{
    char   text[64];
    size_t len = 0;

    // One warm-up call first.  Whatever the first call allocates for its own
    // reasons — a one-time library initialisation, a cached directory handle —
    // is not a leak, and measuring across it would charge that to the function.
    (void)UfUtilsQrCodeCameraGenerate("/dev/null", 640, 480, 0, text, sizeof text, &len);

    long before = OpenDescriptorCount();
    ASSERT_GE(before, 0) << "/proc/self/fd is not readable on this host";

    for (int iteration = 0; iteration < 64; iteration++) {
        ASSERT_EQ(UF_QR_CODE_STATUS_ERR_DEVICE,
                  UfUtilsQrCodeCameraGenerate("/dev/null", 640, 480, 0, text, sizeof text, &len))
            << "iteration " << iteration;
    }

    long after = OpenDescriptorCount();
    ASSERT_GE(after, 0);

    EXPECT_EQ(after, before) << "descriptor count moved by " << (after - before)
                             << " across 64 failing calls — a teardown path is not closing";
}

// The same count over failures that happen at different points in the setup:
// the descriptor is opened in both cases and must be closed in both, which is
// why a leak that only affects one path would be missed by the test above
// alone.
TEST(UfUtilsQrCodeCameraGenerate, DoesNotLeakDescriptorsAcrossMixedFailurePaths)
{
    char   text[64];
    size_t len = 0;
    ScratchFile file("mixed_paths", 0644);

    const char *devices[] = {"/dev/null", file.path(), "/dev/video-does-not-exist-uflib"};

    for (size_t index = 0; index < sizeof devices / sizeof devices[0]; index++) {
        (void)UfUtilsQrCodeCameraGenerate(devices[index], 640, 480, 0, text, sizeof text, &len);
    }

    long before = OpenDescriptorCount();
    ASSERT_GE(before, 0);

    for (int iteration = 0; iteration < 48; iteration++) {
        const char *device = devices[static_cast<size_t>(iteration) % (sizeof devices / sizeof devices[0])];
        ASSERT_EQ(UF_QR_CODE_STATUS_ERR_DEVICE,
                  UfUtilsQrCodeCameraGenerate(device, 640, 480, 0, text, sizeof text, &len));
    }

    EXPECT_EQ(OpenDescriptorCount(), before);
}

// ── It returns ────────────────────────────────────────────────────────────

// A capture call that blocks for ever on a device that never delivers is worse
// than one that fails, and it is a plausible bug in a select() loop whose
// timeout is computed wrongly.  The device here fails before any capture, so
// the bound is generous — it is there to catch a hang, not to measure speed.
TEST(UfUtilsQrCodeCameraGenerate, ReturnsPromptlyWhenTheDeviceCannotBeOpened)
{
    char   text[64];
    size_t len = 0;

    auto started = std::chrono::steady_clock::now();

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_DEVICE,
              UfUtilsQrCodeCameraGenerate("/dev/null", 640, 480, 0, text, sizeof text, &len));

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started).count();

    EXPECT_LT(elapsed_ms, 2000) << "a device that cannot be opened took " << elapsed_ms << "ms";
}

// A generous timeout on an unusable device must not be spent waiting for it:
// the timeout bounds the capture window, and there is no window to bound when
// setup failed.  Ten seconds of timeout returning in under two is the assertion.
TEST(UfUtilsQrCodeCameraGenerate, DoesNotWaitOutTheTimeoutWhenSetupFails)
{
    char   text[64];
    size_t len = 0;

    auto started = std::chrono::steady_clock::now();

    EXPECT_EQ(UF_QR_CODE_STATUS_ERR_DEVICE,
              UfUtilsQrCodeCameraGenerate("/dev/null", 640, 480, 10000, text, sizeof text, &len));

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started).count();

    EXPECT_LT(elapsed_ms, 2000) << "setup failure waited out the capture window: " << elapsed_ms << "ms";
}

// ── A real device, when the host has one ──────────────────────────────────

// This is the only test here that can find a code, and it asserts a defined
// outcome on both kinds of host rather than skipping on one.  With no camera
// the node does not exist and the answer is ERR_DEVICE; with a camera pointed
// at nothing the answer is NO_CODE_FOUND; with a camera pointed at a code it
// would be OK.  All three are observed rather than assumed, so the test cannot
// flake and cannot silently stop testing anything.
TEST(UfUtilsQrCodeCameraGenerate, GivesADefinedAnswerForTheFirstVideoDevice)
{
    if (access("/dev/video0", F_OK) != 0) {
        CameraCall call = Capture("/dev/video0", 640, 480, 0);
        EXPECT_EQ(call.status, UF_QR_CODE_STATUS_ERR_DEVICE);
        return;
    }

    CameraCall call = Capture("/dev/video0", 640, 480, 0);

    EXPECT_TRUE(call.status == UF_QR_CODE_STATUS_OK ||
                call.status == UF_QR_CODE_STATUS_NO_CODE_FOUND ||
                call.status == UF_QR_CODE_STATUS_ERR_DEVICE)
        << "unexpected status " << call.status;

    if (call.status == UF_QR_CODE_STATUS_OK) {
        EXPECT_EQ(call.len, strlen(call.text));
        EXPECT_LT(call.len, sizeof call.text);
    }
}
