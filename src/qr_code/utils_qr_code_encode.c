/**
 * @file utils_qr_code_encode.c
 * @brief QR-code rendering: encode text into a module matrix, via libqrencode.
 *
 * This is a translation unit of its own rather than part of utils_qr_code.c,
 * and the split is load-bearing rather than cosmetic.  A static archive has
 * object granularity, so whatever pulls the decode path out of libuflib.a
 * pulls this object with it if the two share a file — and this file's
 * QRcode_encodeString() reference would then be an undefined symbol on the
 * link line of a consumer that never encodes anything.  uflib sets neither
 * -ffunction-sections nor --gc-sections, so nothing would drop it.  Keeping
 * the encoder separate is what lets a decode-only consumer link -lm and
 * nothing else.
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

#include <qrencode.h>

#include <stdlib.h>
#include <string.h>

/*
 * The widest symbol the specification defines: version 40 is 4*40+17 = 177
 * modules across.  Bound here so that the width*width arithmetic below is
 * provably free of overflow, and so that a library returning something absurd
 * is caught rather than believed.
 */
#define UF_QR_CODE_MAX_SYMBOL_WIDTH 177

/*
 * Payload capacities at version 40, error-correction level L — the largest
 * symbol this function can produce.  Both figures were measured against the
 * libqrencode actually linked here (4.1.1) rather than taken from the table in
 * the specification, by bisecting the longest input each mode accepts.
 *
 * Two figures are needed because libqrencode chooses the densest applicable
 * mode itself and the choice is not ours to predict: a numeric payload packs
 * more than twice as much into the same symbol as an arbitrary byte string.
 * A single byte-mode limit would refuse numeric payloads that encode
 * perfectly well.
 */
#define UF_QR_CODE_MAX_BYTE_PAYLOAD    2953
#define UF_QR_CODE_MAX_NUMERIC_PAYLOAD 7089

UfQrCodeStatus
UfUtilsQrCodeEncodeText(const char *text, UfQrCodeMatrix **out_matrix)
{
    if (text == NULL || out_matrix == NULL) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    /* Cleared before anything can fail, so a caller that ignores the status
       still cannot dereference a previous call's matrix. */
    *out_matrix = NULL;

    size_t length = strlen(text);

    /*
     * The encoder itself refuses the empty string — QRcode_encodeString("")
     * returns NULL on 4.1.1, verified — and nil is not a payload worth a
     * symbol, so it is rejected here as the argument error it is rather than
     * left to surface as an indistinguishable encoder failure.
     */
    if (length == 0) {
        return UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT;
    }

    /* Beyond the densest mode's capacity there is no symbol to be had in any
       mode, so this is decided without troubling the encoder. */
    if (length > UF_QR_CODE_MAX_NUMERIC_PAYLOAD) {
        return UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE;
    }

    /*
     * Version 0 lets libqrencode pick the smallest symbol that fits, level L
     * the most permissive of the four correction levels, QR_MODE_8 asks for
     * byte-mode interpretation of the input.  The mode is a hint rather than a
     * command — the library still packs a numeric payload numerically — which
     * is precisely why the capacity test above cannot assume byte mode.
     */
    QRcode *code = QRcode_encodeString(text, 0, QR_ECLEVEL_L, QR_MODE_8, 1);

    if (code == NULL) {
        /*
         * It fit the widest possible symbol and still failed.  Above the
         * byte-mode limit that leaves only the denser modes unexplored, and
         * the encoder has just declined them, so the payload genuinely does
         * not fit.  At or below it, byte mode was available and was not the
         * obstacle: the failure is internal to the encoder.
         */
        if (length > UF_QR_CODE_MAX_BYTE_PAYLOAD) {
            return UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE;
        }

        return UF_QR_CODE_STATUS_ERR_ENCODE;
    }

    if (code->width <= 0 || (size_t)code->width > UF_QR_CODE_MAX_SYMBOL_WIDTH || code->data == NULL) {
        /* A library reporting a symbol outside the specification has nothing
           worth reading, and its data pointer cannot be sized from a width
           that is not believed. */
        QRcode_free(code);
        return UF_QR_CODE_STATUS_ERR_ENCODE;
    }

    size_t width   = (size_t)code->width;
    size_t modules = width * width;

    /* offsetof, not sizeof: a flexible array member may be preceded by trailing
       padding, and the allocation has to cover the offset the member starts
       at, not the size the type rounds up to. */
    UfQrCodeMatrix *matrix = malloc(offsetof(UfQrCodeMatrix, modules) + modules);

    if (matrix == NULL) {
        QRcode_free(code);
        return UF_QR_CODE_STATUS_ERR_NOMEM;
    }

    matrix->width = width;

    /*
     * Bit 0 is the module and everything above it is a flag.  This was
     * measured rather than assumed: a 21x21 symbol for "hello world" came back
     * holding 0x00, 0x01, 0x02, 0x03, 0x81, 0x84, 0x85, 0x90, 0x91, 0xc0 and
     * 0xc1 — so 289 of its 441 modules are neither 0 nor 1, and a caller
     * testing the byte for truth would render most of the symbol wrongly.
     * Masking here is what makes `if (modules[i])` correct at every call site,
     * and the extraction was checked against the symbol's own finder patterns.
     */
    for (size_t index = 0; index < modules; index++) {
        matrix->modules[index] = (uint8_t)(code->data[index] & 1);
    }

    QRcode_free(code);

    *out_matrix = matrix;

    return UF_QR_CODE_STATUS_OK;
}
