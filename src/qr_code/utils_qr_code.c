/**
 * @file utils_qr_code.c
 * @brief QR-code reading: decode a caller's greyscale image, or capture one
 *        from a V4L2 device and decode that.
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

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/qr_code/utils_qr_code.h>

#include <uflib/quirc/quirc.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#endif

/* ── Geometry validation ─────────────────────────────────────────────────── */

/*
 * quirc does its geometry arithmetic in *signed int*, in three places that are
 * reached before any bound of ours could take effect:
 *
 *   quirc.c:76      size_t newdim = w * h;              (inside quirc_resize,
 *                                                        before the calloc)
 *   identify.c:293  unsigned int numPixels = q->w * q->h;
 *   identify.c:1083 int length = q->w * q->h;
 *
 * A product above INT_MAX is therefore signed integer overflow — undefined
 * behaviour — inside quirc, not merely a wrong pixel count.  Since the ASan
 * preset here enables -fsanitize=signed-integer-overflow, this is a trap a
 * caller can reach with nothing more exotic than a typo'd frame size, so the
 * geometry is refused here rather than handed on.
 *
 * The test is written as a division so that it cannot itself overflow, and
 * each operand is checked against INT_MAX first because quirc's interface
 * takes the dimensions as int.
 */
static bool
sQrCodeGeometryIsProcessable(size_t width, size_t height)
{
    if (width == 0 || height == 0) {
        return false;
    }

    if (width > (size_t)INT_MAX || height > (size_t)INT_MAX) {
        return false;
    }

    if (width > (size_t)INT_MAX / height) {
        return false;
    }

    return true;
}

/* ── Reading a prepared quirc image ──────────────────────────────────────── */

/*
 * quirc_begin() hands back quirc's own image buffer, sized exactly q->w * q->h
 * with a row pitch of q->w — quirc.c:69 allocates it as calloc(w, h) and
 * identify.c:1101 returns q->image directly.  Our caller's stride is theirs to
 * choose and quirc's is not ours to set, so rows move one at a time and the
 * padding beyond `width` is never touched on either side.
 */
static void
sQrCodeCopyGrey(uint8_t *dst, size_t dst_stride, const uint8_t *src, size_t src_stride, size_t width, size_t height)
{
    for (size_t y = 0; y < height; y++) {
        memcpy(dst + y * dst_stride, src + y * src_stride, width);
    }
}

/*
 * Walk every grid quirc found and return the first whose payload decodes.
 *
 * The count is a count of *candidate* finds, not of readable codes: a frame
 * holding two codes of which one is damaged is a frame holding one code, and
 * stopping at index 0 would report the damaged one as nothing at all.
 *
 * Both structs are large — quirc_code carries a QUIRC_MAX_BITMAP cell bitmap
 * of 3917 bytes and quirc_data a QUIRC_MAX_PAYLOAD payload of 8896 — so this
 * frame is about 13 KiB.  They are kept on the stack deliberately: this runs
 * once per captured frame, and two heap allocations per frame in the capture
 * loop buy nothing that 13 KiB of stack does not already provide.
 */
static UfQrCodeStatus
sQrCodeExtractFirstPayload(const struct quirc *qr, char *out_text, size_t out_capacity, size_t *out_len)
{
    struct quirc_code code;
    struct quirc_data data;
    int count = quirc_count(qr);

    for (int index = 0; index < count; index++) {
        quirc_extract(qr, index, &code);

        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
            continue;
        }

        /*
         * quirc clamps and NUL-terminates the payload itself (decode.c:876-878),
         * so a length outside the array means its own bookkeeping is wrong and
         * trusting it would read past the struct.  Treated as "this candidate
         * did not decode" rather than as an error: the next candidate may be
         * perfectly good.
         */
        if (data.payload_len < 0 || (size_t)data.payload_len >= sizeof(data.payload)) {
            continue;
        }

        size_t needed = (size_t)data.payload_len + 1;
        if (needed > out_capacity) {
            *out_len = needed;
            return UF_QR_CODE_STATUS_ERR_BUFFER_TOO_SMALL;
        }

        memcpy(out_text, data.payload, (size_t)data.payload_len);
        out_text[data.payload_len] = '\0';
        *out_len = (size_t)data.payload_len;

        return UF_QR_CODE_STATUS_OK;
    }

    return UF_QR_CODE_STATUS_NO_CODE_FOUND;
}

/* ── Decode ──────────────────────────────────────────────────────────────── */

UfQrCodeStatus
UfUtilsQrCodeDecode(const uint8_t *image, size_t width, size_t height, size_t stride,
                    char *out_text, size_t out_capacity, size_t *out_len)
{
    if (image == NULL || out_text == NULL || out_len == NULL || out_capacity == 0) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    if (!sQrCodeGeometryIsProcessable(width, height)) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    /* Rows may be padded, never overlapped: a stride below the row width would
       mean consecutive rows share pixels, which no producer emits and which
       would decode a lattice nobody photographed. */
    if (stride < width) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    struct quirc *qr = quirc_new();
    if (qr == NULL) {
        return UF_QR_CODE_STATUS_ERR_NOMEM;
    }

    UfQrCodeStatus status;

    if (quirc_resize(qr, (int)width, (int)height) < 0) {
        /* Only allocation can fail here.  The geometry was already validated
           to be non-negative and inside int range, which is the whole of
           quirc_resize's other rejection path (quirc.c:62). */
        status = UF_QR_CODE_STATUS_ERR_NOMEM;
    } else {
        int qr_width = 0;
        int qr_height = 0;
        uint8_t *const pixels = quirc_begin(qr, &qr_width, &qr_height);

        sQrCodeCopyGrey(pixels, (size_t)qr_width, image, stride, width, height);
        quirc_end(qr);

        status = sQrCodeExtractFirstPayload(qr, out_text, out_capacity, out_len);
    }

    quirc_destroy(qr);

    return status;
}

/* ── Camera capture (V4L2) ───────────────────────────────────────────────── */

#ifdef __linux__

#define UF_QR_CODE_CAMERA_BUFFER_COUNT 2

/*
 * A single-frame call (timeout_ms == 0) still has to wait for a frame to
 * arrive, and "wait for the first frame" has no natural bound: a device needs
 * a moment after STREAMON, and a device that never delivers anything must not
 * hang the caller for ever.  This is that bound — generous for a 30 fps
 * device, short enough that a wedged camera is reported rather than waited on.
 */
#define UF_QR_CODE_CAMERA_FIRST_FRAME_GRACE_MS 1000

typedef struct UfQrCodeCamera {
    int      fd;
    unsigned buffer_count;
    void    *buffers[UF_QR_CODE_CAMERA_BUFFER_COUNT];
    size_t   lengths[UF_QR_CODE_CAMERA_BUFFER_COUNT];
    size_t   width;
    size_t   height;
    size_t   stride;
    bool     streaming;
} UfQrCodeCamera;

/*
 * V4L2 requires a retry on EINTR: an ioctl interrupted by a signal has not
 * been performed, and treating it as a failure would report a device error for
 * something as ordinary as a SIGWINCH arriving mid-capture.  Every other errno
 * is a real failure and is returned as-is, with errno left set for the caller.
 */
static int
sQrCodeIoctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);

    return result;
}

/*
 * CLOCK_MONOTONIC, because the capture window must not be moved by anything
 * that steps the wall clock.  If the read fails — which needs a syscall
 * failure rather than a clock problem — the second-resolution fallback keeps
 * the deadline arithmetic meaningful.  Returning 0 here instead would make the
 * capture loop's `now >= deadline` test permanently false and spin for ever,
 * so the fallback is a correctness measure, not a convenience.
 */
static uint64_t
sQrCodeMonotonicMillis(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        return (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000);
    }

    return (uint64_t)time(NULL) * 1000u;
}

/*
 * YUYV packs two pixels into four bytes as Y0 U Y1 V, so the luminance byte of
 * pixel x always sits at byte offset 2*x whatever the parity — the chroma bytes
 * occupy the odd offsets and are never read.  Colours are not interpreted:
 * QR recognition is a luminance problem, and discarding chroma is also what
 * keeps this loop branch-free.
 */
static void
sQrCodeCopyYuyv(uint8_t *dst, size_t dst_stride, const uint8_t *src, size_t src_stride, size_t width, size_t height)
{
    for (size_t y = 0; y < height; y++) {
        const uint8_t *row = src + y * src_stride;
        uint8_t       *out = dst + y * dst_stride;

        for (size_t x = 0; x < width; x++) {
            out[x] = row[x * 2];
        }
    }
}

/*
 * Open the device, negotiate YUYV at the requested geometry, and map its
 * buffers.  Every failure path leaves the descriptor closed and nothing
 * mapped, so the caller has a single teardown to perform regardless of how far
 * this got — which is why the teardown is unconditional rather than paired
 * with each step.
 *
 * A failing syscall leaves errno describing itself, which the caller surfaces.
 * The paths that fail on the driver's *answer* rather than on a failed call
 * set errno explicitly, so that the documented contract — errno is meaningful
 * whenever ERR_DEVICE is returned — does not quietly depend on which step
 * happened to fail.
 */
static UfQrCodeStatus
sQrCodeCameraOpen(UfQrCodeCamera *camera, const char *device, size_t width, size_t height)
{
    /* O_CLOEXEC: a library must not hand a captured descriptor to whatever the
       host later execs. */
    camera->fd = open(device, O_RDWR | O_CLOEXEC);
    if (camera->fd < 0) {
        return UF_QR_CODE_STATUS_ERR_DEVICE;
    }

    struct v4l2_capability capability;

    memset(&capability, 0, sizeof capability);
    if (sQrCodeIoctl(camera->fd, VIDIOC_QUERYCAP, &capability) < 0) {
        return UF_QR_CODE_STATUS_ERR_DEVICE;
    }

    /*
     * This is what separates a capture device from a file that merely opened:
     * /dev/null and a regular file both open happily and both fail here with
     * ENOTTY from the ioctl, which is the check that makes the status honest
     * rather than a guess.
     *
     * device_caps is the per-node answer and supersedes capabilities whenever
     * the driver reports it; on a device node the two can differ, and it is
     * the node being opened that has to support capture.
     */
    uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                        ? capability.device_caps
                        : capability.capabilities;

    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        errno = ENODEV;
        return UF_QR_CODE_STATUS_ERR_DEVICE;
    }

    struct v4l2_format format;

    memset(&format, 0, sizeof format);
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = (uint32_t)width;
    format.fmt.pix.height = (uint32_t)height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_NONE;

    if (sQrCodeIoctl(camera->fd, VIDIOC_S_FMT, &format) < 0) {
        return UF_QR_CODE_STATUS_ERR_DEVICE;
    }

    /*
     * S_FMT writes back what the driver actually granted, so no VIDIOC_G_FMT
     * round trip is needed to learn it.  Both substitutions have to be caught:
     * a driver that answers in MJPEG would have its bytes read as YUYV and
     * decoded into noise, and one that answers at a different geometry would
     * have its rows read at the wrong pitch.
     */
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        errno = EINVAL;
        return UF_QR_CODE_STATUS_ERR_DEVICE;
    }

    camera->width  = format.fmt.pix.width;
    camera->height = format.fmt.pix.height;

    /* bytesperline is a request-driven value some drivers leave at zero, in
       which case the packed YUYV row is exactly two bytes per pixel. */
    camera->stride = format.fmt.pix.bytesperline
                         ? format.fmt.pix.bytesperline
                         : camera->width * 2;

    /* YUYV needs two bytes per pixel; a narrower line cannot hold the row, and
       reading it as though it could would walk off the end of the buffer. */
    if (camera->stride < camera->width * 2) {
        errno = EINVAL;
        return UF_QR_CODE_STATUS_ERR_DEVICE;
    }

    struct v4l2_requestbuffers request;

    memset(&request, 0, sizeof request);
    request.count = UF_QR_CODE_CAMERA_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    if (sQrCodeIoctl(camera->fd, VIDIOC_REQBUFS, &request) < 0) {
        return UF_QR_CODE_STATUS_ERR_DEVICE;
    }

    /* The driver may grant fewer than asked.  Two is the floor for streaming:
       with one buffer there is nothing to hand back while the next frame
       fills, and STREAMON is refused. */
    if (request.count < 2) {
        errno = ENOSPC;
        return UF_QR_CODE_STATUS_ERR_DEVICE;
    }

    camera->buffer_count = request.count;

    for (unsigned index = 0; index < camera->buffer_count; index++) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof buffer);
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;

        if (sQrCodeIoctl(camera->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            return UF_QR_CODE_STATUS_ERR_DEVICE;
        }

        camera->lengths[index] = buffer.length;
        camera->buffers[index] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                      camera->fd, buffer.m.offset);

        if (camera->buffers[index] == MAP_FAILED) {
            /* Recorded as unmapped so the teardown's munmap loop skips it —
               MAP_FAILED is not a mapping, and passing it to munmap would be
               undefined. */
            camera->buffers[index] = NULL;
            return UF_QR_CODE_STATUS_ERR_DEVICE;
        }
    }

    for (unsigned index = 0; index < camera->buffer_count; index++) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof buffer);
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;

        if (sQrCodeIoctl(camera->fd, VIDIOC_QBUF, &buffer) < 0) {
            return UF_QR_CODE_STATUS_ERR_DEVICE;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (sQrCodeIoctl(camera->fd, VIDIOC_STREAMON, &type) < 0) {
        return UF_QR_CODE_STATUS_ERR_DEVICE;
    }

    camera->streaming = true;

    return UF_QR_CODE_STATUS_OK;
}

/*
 * Read frames until one decodes or the window closes.  `timeout_ms` bounds the
 * capture window only: the device is already open and streaming when this is
 * called, so setup time is not charged against it.
 */
static UfQrCodeStatus
sQrCodeCameraCapture(UfQrCodeCamera *camera, struct quirc *qr, unsigned timeout_ms,
                     char *out_text, size_t out_capacity, size_t *out_len)
{
    bool     single_frame = (timeout_ms == 0);
    uint64_t budget_ms    = single_frame ? UF_QR_CODE_CAMERA_FIRST_FRAME_GRACE_MS : timeout_ms;
    uint64_t deadline     = sQrCodeMonotonicMillis() + budget_ms;

    for (;;) {
        uint64_t now = sQrCodeMonotonicMillis();

        if (now >= deadline) {
            return UF_QR_CODE_STATUS_NO_CODE_FOUND;
        }

        uint64_t remaining_ms = deadline - now;

        struct timeval timeout;

        timeout.tv_sec  = (time_t)(remaining_ms / 1000);
        timeout.tv_usec = (suseconds_t)((remaining_ms % 1000) * 1000);

        fd_set read_fds;

        FD_ZERO(&read_fds);
        FD_SET(camera->fd, &read_fds);

        int ready = select(camera->fd + 1, &read_fds, NULL, NULL, &timeout);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;   /* a signal, not an empty window — the deadline decides */
            }
            return UF_QR_CODE_STATUS_ERR_DEVICE;
        }

        if (ready == 0) {
            return UF_QR_CODE_STATUS_NO_CODE_FOUND;
        }

        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof buffer);
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;

        if (sQrCodeIoctl(camera->fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                continue;   /* select said ready, the driver disagreed */
            }
            return UF_QR_CODE_STATUS_ERR_DEVICE;
        }

        if (buffer.index >= camera->buffer_count) {
            /* Not reachable through a conforming driver, and not survivable if
               it were: the index is used to select a mapping and a length. */
            errno = EIO;
            return UF_QR_CODE_STATUS_ERR_DEVICE;
        }

        int qr_width = 0;
        int qr_height = 0;
        uint8_t *const pixels = quirc_begin(qr, &qr_width, &qr_height);

        sQrCodeCopyYuyv(pixels, (size_t)qr_width,
                        camera->buffers[buffer.index], camera->stride,
                        camera->width, camera->height);

        /*
         * Hand the buffer straight back before decoding.  Decoding a frame is
         * milliseconds of work and the device has only two buffers, so holding
         * this one across it would stall capture and drop the frames the rest
         * of the window is meant to look at.
         */
        if (sQrCodeIoctl(camera->fd, VIDIOC_QBUF, &buffer) < 0) {
            return UF_QR_CODE_STATUS_ERR_DEVICE;
        }

        quirc_end(qr);

        UfQrCodeStatus status = sQrCodeExtractFirstPayload(qr, out_text, out_capacity, out_len);

        if (status != UF_QR_CODE_STATUS_NO_CODE_FOUND) {
            return status;
        }

        /* A single-frame call examines one frame whatever that frame held;
           only a windowed call keeps looking. */
        if (single_frame) {
            return UF_QR_CODE_STATUS_NO_CODE_FOUND;
        }
    }
}

/*
 * Unconditional teardown.  Three returns below are deliberately unchecked, and
 * each for the same reason: the descriptor is closed at the end of this
 * function, which stops streaming and releases the mappings whatever these
 * report, so there is no recovery to attempt and no state left to correct.
 * The alternative — propagating a teardown failure — would let a successful
 * capture be reported as a failure.
 */
static void
sQrCodeCameraClose(UfQrCodeCamera *camera)
{
    if (camera->fd < 0) {
        return;
    }

    if (camera->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        (void)sQrCodeIoctl(camera->fd, VIDIOC_STREAMOFF, &type);
    }

    for (unsigned index = 0; index < camera->buffer_count; index++) {
        if (camera->buffers[index] != NULL) {
            (void)munmap(camera->buffers[index], camera->lengths[index]);
            camera->buffers[index] = NULL;
        }
    }

    (void)close(camera->fd);
    camera->fd = -1;
    camera->buffer_count = 0;
    camera->streaming = false;
}

UfQrCodeStatus
UfUtilsQrCodeCameraGenerate(const char *device, int width, int height, unsigned timeout_ms,
                            char *out_text, size_t out_capacity, size_t *out_len)
{
    if (device == NULL || *device == '\0' || out_text == NULL || out_len == NULL || out_capacity == 0) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    if (width <= 0 || height <= 0) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    if (!sQrCodeGeometryIsProcessable((size_t)width, (size_t)height)) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    struct quirc *qr = quirc_new();
    if (qr == NULL) {
        return UF_QR_CODE_STATUS_ERR_NOMEM;
    }

    UfQrCodeStatus status;

    UfQrCodeCamera camera;

    memset(&camera, 0, sizeof camera);
    camera.fd = -1;

    errno = 0;
    status = sQrCodeCameraOpen(&camera, device, (size_t)width, (size_t)height);

    if (status == UF_QR_CODE_STATUS_OK) {
        /*
         * Resized to the geometry the driver *granted*, not the one asked for,
         * because that is what the copy in the capture loop writes.  Checked
         * again first for the same reason the request was: these are a
         * driver's numbers rather than a caller's, and a frame size above
         * INT_MAX reaches the identical signed overflow inside quirc_resize.
         */
        if (!sQrCodeGeometryIsProcessable(camera.width, camera.height)) {
            errno = EINVAL;
            status = UF_QR_CODE_STATUS_ERR_DEVICE;
        } else if (quirc_resize(qr, (int)camera.width, (int)camera.height) < 0) {
            status = UF_QR_CODE_STATUS_ERR_NOMEM;
        }
    }

    if (status == UF_QR_CODE_STATUS_OK) {
        status = sQrCodeCameraCapture(&camera, qr, timeout_ms, out_text, out_capacity, out_len);
    }

    /* Captured before the teardown, whose own syscalls would otherwise
       overwrite it with a cause that has nothing to do with the failure. */
    int cause = errno;

    sQrCodeCameraClose(&camera);
    quirc_destroy(qr);

    errno = cause;

    return status;
}

#else /* !__linux__ */

UfQrCodeStatus
UfUtilsQrCodeCameraGenerate(const char *device, int width, int height, unsigned timeout_ms,
                            char *out_text, size_t out_capacity, size_t *out_len)
{
    /*
     * Arguments are still validated before the platform is refused.  A caller
     * with two mistakes should be told about the one that is wrong on every
     * platform, and a test covering the argument checks should not have to be
     * Linux-only to exercise them.
     */
    if (device == NULL || *device == '\0' || out_text == NULL || out_len == NULL || out_capacity == 0) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    if (width <= 0 || height <= 0) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    if (!sQrCodeGeometryIsProcessable((size_t)width, (size_t)height)) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    (void)timeout_ms;

    return UF_QR_CODE_STATUS_ERR_UNSUPPORTED;
}

#endif /* __linux__ */
