/**
 * @file utils_qr_code.h
 * @brief QR-code reading and rendering: decode an image, capture one from a
 *        V4L2 camera, or encode text into a module matrix.
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

#ifndef UFLIB_QR_CODE_UTILS_QR_CODE_H
#define UFLIB_QR_CODE_UTILS_QR_CODE_H

#include <uflib/uflib_defs.h>

#include <uflib/qr_code/utils_qr_code_type.h>

/**
 * @brief Read the QR code in a greyscale image.
 *
 * The image is a single-channel 8-bit raster, one luminance byte per pixel,
 * with @p stride bytes between the start of one row and the next.  A stride
 * wider than @p width is normal — capture buffers are usually padded to an
 * alignment — and the padding bytes are never read.  A stride narrower than
 * @p width is rejected: it would mean rows overlapping, which no producer
 * emits and which would silently decode the wrong pixels.
 *
 * Nothing is written to @p out_text unless a code was read, so a caller may
 * hold a previous result in the buffer across a failed call.
 *
 * @param[in]  image         Greyscale pixels.  Must be non-NULL.
 * @param[in]  width         Pixels per row.  Must be > 0.
 * @param[in]  height        Rows in the image.  Must be > 0.
 * @param[in]  stride        Bytes per row.  Must be >= @p width.
 * @param[out] out_text      Receives the payload and a NUL terminator.
 * @param[in]  out_capacity  Bytes available at @p out_text.  Must be > 0.
 * @param[out] out_len       Receives the payload length, excluding the
 *                           terminator.  Must be non-NULL.
 *
 * @return @ref UF_QR_CODE_STATUS_OK when a code was read and its payload was
 *         copied; @ref UF_QR_CODE_STATUS_NO_CODE_FOUND when the image held no
 *         readable code; @ref UF_QR_CODE_STATUS_ERR_BUFFER_TOO_SMALL when the
 *         payload does not fit, in which case @p *out_len is set to the size
 *         required including the terminator, so a caller may retry with a
 *         correctly sized buffer; or @ref UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT
 *         / @ref UF_QR_CODE_STATUS_ERR_NOMEM on the conditions named above.
 *
 * @note Ownership: nothing is allocated.  @p out_text is the caller's
 *       throughout, and on failure @p *out_len is left set only for the
 *       too-small case described above.
 *
 * @warning The image geometry is validated against an internal working buffer
 *          whose extent is computed in bytes, not in
 *          @c width*height pixels.  An image too large to process is refused
 *          with @ref UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT rather than
 *          truncated, so a caller cannot mistake a partial decode for a
 *          complete one.
 *
 * @code{.c}
 * char   text[512];
 * size_t len;
 *
 * UfQrCodeStatus status = UfUtilsQrCodeDecode(grey, 640, 480, 640, text, sizeof text, &len);
 * if (status == UF_QR_CODE_STATUS_OK) {
 *     printf("payload: %.*s\n", (int)len, text);
 * } else if (status == UF_QR_CODE_STATUS_NO_CODE_FOUND) {
 *     // the image was fine; there was simply no code in it
 * }
 * @endcode
 */
PUBLIC_API UfQrCodeStatus UfUtilsQrCodeDecode(const uint8_t *image, size_t width, size_t height, size_t stride,
                                              char *out_text, size_t out_capacity, size_t *out_len);

/**
 * @brief Open a V4L2 capture device, read frames from it, and return the text
 *        of the first QR code found.
 *
 * The device is opened, configured, streamed, torn down and closed within this
 * call — the caller passes a path, not a handle, and holds no state between
 * calls.  Every exit path releases the buffers and closes the descriptor,
 * including the error paths.
 *
 * Frames are captured in YUYV 4:2:2 and reduced to luminance internally, so a
 * caller never handles a pixel format.  @p width and @p height are a
 * *request*, not a guarantee: a driver may negotiate something else, and the
 * geometry actually granted is what gets decoded.
 *
 * On a platform without V4L2 the function is still present and returns
 * @ref UF_QR_CODE_STATUS_ERR_UNSUPPORTED — see @ref UfQrCodeStatus.
 *
 * @param[in]  device        Path to the capture device, e.g. @c "/dev/video0".
 *                           Must be non-NULL and non-empty; there is no
 *                           default device, so a mistyped path fails loudly
 *                           rather than silently opening the wrong camera.
 * @param[in]  width         Requested frame width in pixels.  Must be > 0.
 * @param[in]  height        Requested frame height in pixels.  Must be > 0.
 * @param[in]  timeout_ms    How long to keep reading frames, in milliseconds.
 *                           Zero means a single frame.
 * @param[out] out_text      Receives the payload and a NUL terminator.
 * @param[in]  out_capacity  Bytes available at @p out_text.  Must be > 0.
 * @param[out] out_len       Receives the payload length, excluding the
 *                           terminator.  Must be non-NULL.
 *
 * @return @ref UF_QR_CODE_STATUS_OK when a code was read;
 *         @ref UF_QR_CODE_STATUS_NO_CODE_FOUND when the window elapsed with
 *         frames arriving but no code in them; @ref UF_QR_CODE_STATUS_ERR_DEVICE
 *         when the device could not be opened, is not a video capture device,
 *         or refused a request; @ref UF_QR_CODE_STATUS_ERR_UNSUPPORTED on a
 *         platform without V4L2; or @ref UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT
 *         / @ref UF_QR_CODE_STATUS_ERR_BUFFER_TOO_SMALL / @ref
 *         UF_QR_CODE_STATUS_ERR_NOMEM as for @ref UfUtilsQrCodeDecode.
 *
 * @note Every frame that arrives inside the window is examined; none is
 *       skipped.  With @p timeout_ms of 0 a single frame is read, which is the
 *       per-frame entry point for a caller driving its own display loop.
 *
 * @note When @ref UF_QR_CODE_STATUS_ERR_DEVICE is returned, @c errno is left
 *       set to the cause: the failing call's own value where a syscall failed,
 *       and @c ENODEV, @c EINVAL, @c ENOSPC or @c EIO where the driver's
 *       answer was the problem.  It carries no meaning for any other status.
 *       A camera that opens but reports the wrong pixel format therefore
 *       reports @c EINVAL rather than appearing as an unexplained device
 *       error.
 *
 * @warning Opening and configuring a device costs a handful of ioctls and two
 *          buffer mappings, so calling this per displayed frame is materially
 *          more expensive than calling @ref UfUtilsQrCodeDecode per frame on a
 *          stream the caller already holds.
 *
 * @code{.c}
 * char   text[512];
 * size_t len;
 *
 * UfQrCodeStatus status = UfUtilsQrCodeCameraGenerate("/dev/video0", 640, 480,
 *                                                     2000, text, sizeof text, &len);
 * if (status == UF_QR_CODE_STATUS_OK) {
 *     printf("scanned: %.*s\n", (int)len, text);
 * }
 * @endcode
 */
PUBLIC_API UfQrCodeStatus UfUtilsQrCodeCameraGenerate(const char *device, int width, int height, unsigned timeout_ms,
                                                      char *out_text, size_t out_capacity, size_t *out_len);

/**
 * @brief Encode text as a QR symbol and return its module matrix.
 *
 * The result is a single allocation holding the matrix header followed by
 * @c width*width module bytes, so the caller frees it with one @c free().
 * Nothing is returned unless encoding succeeded: on failure @p *out_matrix is
 * set to NULL, so a caller cannot dereference a stale pointer after a failed
 * call.
 *
 * The symbol is written at error-correction level L, the most permissive of
 * the four, giving the smallest symbol for a payload and the most tolerance
 * for a camera that is not held square to the code.  A caller that needs a
 * denser code should not be using this function's fixed choice.
 *
 * @param[in]  text        NUL-terminated payload.  Must be non-NULL and
 *                         non-empty.  Encoded as bytes, not as text: the
 *                         string is read up to its first NUL, so embedded NUL
 *                         bytes truncate the payload rather than being carried.
 * @param[out] out_matrix  Receives the malloc'd matrix.  Must be non-NULL.
 *
 * @return @ref UF_QR_CODE_STATUS_OK on success;
 *         @ref UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE when the payload needs a
 *         symbol beyond the specification's largest;
 *         @ref UF_QR_CODE_STATUS_ERR_ENCODE when the encoder refused the input
 *         or its output could not be read back;
 *         @ref UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT or
 *         @ref UF_QR_CODE_STATUS_ERR_NOMEM on the conditions named above.
 *
 * @note Ownership: on success @p *out_matrix is malloc'd and must be freed by
 *       the caller with @c free().  On failure it is NULL and there is nothing
 *       to free.
 *
 * @warning The matrix carries no quiet zone — it is the symbol's data region
 *          only.  A caller that renders it for a scanner to read must add the
 *          light border the specification requires, or the code will not
 *          decode however correct the modules are.
 *
 * @code{.c}
 * UfQrCodeMatrix *matrix = NULL;
 * if (UfUtilsQrCodeEncodeText("https://unfacd.io", &matrix) == UF_QR_CODE_STATUS_OK) {
 *     for (size_t y = 0; y < matrix->width; y++) {
 *         for (size_t x = 0; x < matrix->width; x++) {
 *             putchar(matrix->modules[y * matrix->width + x] ? '#' : ' ');
 *         }
 *         putchar('\n');
 *     }
 *     free(matrix);
 * }
 * @endcode
 */
PUBLIC_API UfQrCodeStatus UfUtilsQrCodeEncodeText(const char *text, UfQrCodeMatrix **out_matrix);

#endif //UFLIB_QR_CODE_UTILS_QR_CODE_H
