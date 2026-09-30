/**
 * @file utils_qr_code_type.h
 * @brief The QR-code module's public types: the status every entry point
 *        returns, and the module matrix the encoder hands back.
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

#ifndef UFLIB_QR_CODE_UTILS_QR_CODE_TYPE_H
#define UFLIB_QR_CODE_UTILS_QR_CODE_TYPE_H

#include <uflib/uflib_defs.h>

#include <uflib/standard_c_includes.h>

/**
 * @brief The outcome of every qr_code entry point.
 *
 * ## The sign of the value is part of the contract
 *
 * @ref UF_QR_CODE_STATUS_NO_CODE_FOUND is deliberately **positive**, not an
 * error.  It reports that an image was examined in full and held no readable
 * code — a normal event when a camera is pointed at a wall.  A capture loop
 * written as
 *
 * @code{.c}
 * for (;;) {
 *     UfQrCodeStatus status = UfUtilsQrCodeCameraGenerate("/dev/video0", 640, 480, 0,
 *                                                         text, sizeof text, &len);
 *     if (status < 0) {           // ← correct only because NO_CODE_FOUND is positive
 *         return status;          //   a device error aborts; an empty frame does not
 *     }
 *     if (status == UF_QR_CODE_STATUS_OK) {
 *         handle(text, len);
 *     }
 * }
 * @endcode
 *
 * must therefore keep spinning on an empty frame.  Every negative value is a
 * real error, and every error is negative — that is the whole rule.
 *
 * ## Every value here is reachable
 *
 * There is deliberately no code for "the capability is off".  When a
 * capability is off the functions are not compiled at all, so a caller cannot
 * hold a pointer to one and ask whether it works: the question is answered at
 * link time, not run time.  @ref UF_QR_CODE_STATUS_ERR_UNSUPPORTED therefore
 * means exactly one thing — the camera was asked for on a platform that has no
 * V4L2 — and not "this build lacks quirc".
 */
typedef enum UfQrCodeStatus {
  UF_QR_CODE_STATUS_OK = 0,                            ///< A code was recovered; @c out_text holds its payload.
  UF_QR_CODE_STATUS_NO_CODE_FOUND = 1,                 ///< The image was examined and no readable code was present.  Not an error — see above.
  UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT = -1,         ///< A NULL, empty or out-of-range argument, or a geometry too large to process.
  UF_QR_CODE_STATUS_ERR_BUFFER_TOO_SMALL = -2,         ///< The caller's text buffer cannot hold the payload; the size needed is in @c *out_len.
  UF_QR_CODE_STATUS_ERR_NOMEM = -3,                    ///< Allocation failed.
  UF_QR_CODE_STATUS_ERR_ENCODE = -4,                   ///< The encoder declined the input, or its output could not be read back.
  UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE = -5,         ///< The text needs a QR symbol larger than the specification allows.  Distinct from @ref UF_QR_CODE_STATUS_ERR_NOMEM on purpose: retrying will not help.
  UF_QR_CODE_STATUS_ERR_DEVICE = -6,                   ///< The capture device could not be opened, is not a video device, or refused a V4L2 request.
  UF_QR_CODE_STATUS_ERR_UNSUPPORTED = -7               ///< Capture was requested on a platform without V4L2.  See the note above.
} UfQrCodeStatus;

/**
 * @brief A rendered QR symbol: one byte per module, row-major.
 *
 * Returned by @ref UfUtilsQrCodeEncodeText in a single allocation, so the
 * caller releases it with one @c free() and never needs a destructor of its
 * own — the same ownership contract @c hex2bin() documents in `utils_hex.h`.
 *
 * @c modules is a flexible array member sized @c width*width, so the square
 * geometry is implied rather than carried twice.  A module is 0 when light and
 * 1 when dark: the encoder's own bit 0 is normalised away at the boundary, so
 * a caller rendering the matrix tests it as a boolean and never writes the
 * @c "& 1" that the underlying library would otherwise require.
 *
 * @note There is no quiet zone.  The matrix is the symbol's data region only;
 *       a caller rendering it for a scanner to read must add the four-module
 *       light border the specification requires.  Omitting it is the most
 *       common reason a freshly rendered code will not decode.
 */
typedef struct UfQrCodeMatrix {
  size_t  width;    ///< Modules per side; the symbol is @c width x @c width.
  uint8_t modules[]; ///< Flexible array member — @c width*width bytes, row-major, 0 = light, 1 = dark.
} UfQrCodeMatrix;

#endif //UFLIB_QR_CODE_UTILS_QR_CODE_TYPE_H
