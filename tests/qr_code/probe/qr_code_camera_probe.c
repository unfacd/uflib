/**
 * @file qr_code_camera_probe.c
 * @brief Point a camera at a QR code and print what was read.
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

/*
 * A consumer of the camera path and nothing else.
 *
 * It is deliberately decode-only: it links no encoder, so it exercises the
 * half of the module that reads a camera and none of the half that draws a
 * symbol.  The ncurses demo cannot make that claim — it renders whatever it
 * read by encoding it again, so a correct decode of a corrupt payload and a
 * corrupt decode both end up as a wrong picture.  Here the payload is printed
 * as bytes and there is nothing between the camera and the output.
 *
 * What it distinguishes, which is the whole reason it exists:
 *
 *   device error      the path is wrong, or the node is not a capture device
 *                     — reported at once with errno, not after the timeout
 *   frames arriving   a dot per frame, no code in any of them: the camera is
 *                     live and the symbol is the problem — too small in frame,
 *                     out of focus, over-exposed, or the quiet zone is clipped
 *   code read         the payload, escaped, with its exact length
 *
 * Build:  cmake -DUFLIB_CAPABILITY_QUIRC=ON -D_PACKAGE_TESTS=ON …
 *         cmake --build <dir> --target qr_code_camera_probe
 * Run:    ./qr_code_camera_probe [/dev/video0] [seconds] [width] [height]
 *
 * Exit:   0 code read   1 no code within the window   2 the device refused
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/qr_code/utils_qr_code.h>

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PROBE_DEFAULT_DEVICE   "/dev/video0"
#define PROBE_DEFAULT_SECONDS  30
#define PROBE_DEFAULT_WIDTH    640
#define PROBE_DEFAULT_HEIGHT   480

/* Generous on purpose: a payload that does not fit should be a finding about
   the symbol's contents, not the first thing this program reports. */
#define PROBE_TEXT_CAPACITY    4096

/* A dot per frame would be unreadable at thirty frames a second, so the
   heartbeat is throttled to roughly ten a second. */
#define PROBE_DOT_INTERVAL_MS  100

typedef enum ProbeExit {
    PROBE_EXIT_READ         = 0,
    PROBE_EXIT_NO_CODE      = 1,
    PROBE_EXIT_DEVICE       = 2,
    PROBE_EXIT_USAGE        = 64,
} ProbeExit;

static uint64_t
sNowMs(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/*
 * Print the payload as bytes rather than as text.  A scanner that reads a code
 * carrying a trailing newline, or a stray control byte, is otherwise
 * indistinguishable from one that read the string you expected — and the
 * difference matters the moment the payload is compared against anything.
 */
static void
sPrintEscaped(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];

        switch (c) {
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (c >= 0x20 && c < 0x7f) {
                    putchar(c);
                } else {
                    printf("\\x%02x", c);
                }
                break;
        }
    }
}

static void
sReportStatus(UfQrCodeStatus status)
{
    switch (status) {
        case UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT:
            fprintf(stderr, "probe: the library rejected the arguments it was given\n");
            break;

        case UF_QR_CODE_STATUS_ERR_BUFFER_TOO_SMALL:
            fprintf(stderr, "probe: a code was read but its payload does not fit %d bytes\n",
                    PROBE_TEXT_CAPACITY);
            break;

        case UF_QR_CODE_STATUS_ERR_NOMEM:
            fprintf(stderr, "probe: out of memory\n");
            break;

        case UF_QR_CODE_STATUS_ERR_UNSUPPORTED:
            fprintf(stderr, "probe: this build has no V4L2 capture support\n");
            break;

        default:
            fprintf(stderr, "probe: unexpected status %d\n", (int)status);
            break;
    }
}

static void
sUsage(const char *program)
{
    fprintf(stderr,
            "usage: %s [device] [seconds] [width] [height]\n"
            "       device   default " PROBE_DEFAULT_DEVICE "\n"
            "       seconds  default %d, 0 for a single attempt\n"
            "       width    default %d\n"
            "       height   default %d\n",
            program, PROBE_DEFAULT_SECONDS, PROBE_DEFAULT_WIDTH, PROBE_DEFAULT_HEIGHT);
}

int
main(int argc, char **argv)
{
    const char *device = (argc > 1) ? argv[1] : PROBE_DEFAULT_DEVICE;
    int seconds = (argc > 2) ? atoi(argv[2]) : PROBE_DEFAULT_SECONDS;
    int width = (argc > 3) ? atoi(argv[3]) : PROBE_DEFAULT_WIDTH;
    int height = (argc > 4) ? atoi(argv[4]) : PROBE_DEFAULT_HEIGHT;

    if (argc > 5) {
        sUsage(argv[0]);
        return PROBE_EXIT_USAGE;
    }

    if (device[0] == '\0' || seconds < 0 || width <= 0 || height <= 0) {
        sUsage(argv[0]);
        return PROBE_EXIT_USAGE;
    }

    fprintf(stderr, "probe: %s, %dx%d requested, %d second%s\n",
            device, width, height, seconds, seconds == 1 ? "" : "s");

    uint64_t start = sNowMs();
    uint64_t deadline = start + (uint64_t)seconds * 1000u;
    uint64_t last_dot = start;

    unsigned attempts = 0;
    unsigned without_code = 0;

    char   text[PROBE_TEXT_CAPACITY];
    size_t length = 0;

    for (;;) {
        errno = 0;

        UfQrCodeStatus status = UfUtilsQrCodeCameraGenerate(device, width, height, 0,
                                                            text, sizeof text, &length);

        /* Read before anything else, while it still describes this call. */
        int cause = errno;

        attempts++;

        if (status == UF_QR_CODE_STATUS_OK) {
            uint64_t elapsed = sNowMs() - start;

            if (attempts > 1 && last_dot != start) {
                fputc('\n', stderr);
            }

            printf("read after %u frame%s in %llu ms\n",
                   attempts, attempts == 1 ? "" : "s", (unsigned long long)elapsed);
            printf("length: %zu\n", length);
            printf("payload: ");
            sPrintEscaped(text, length);
            printf("\n");

            /*
             * Worth calling out explicitly: a payload that arrives with
             * surrounding whitespace reads as a correct scan and fails every
             * comparison a caller makes against it.
             */
            if (length > 0 &&
                (isspace((unsigned char)text[0]) || isspace((unsigned char)text[length - 1]))) {
                printf("note: the payload has leading or trailing whitespace\n");
            }

            return PROBE_EXIT_READ;
        }

        if (status == UF_QR_CODE_STATUS_NO_CODE_FOUND) {
            without_code++;

            uint64_t now = sNowMs();

            if (now - last_dot >= PROBE_DOT_INTERVAL_MS) {
                fputc('.', stderr);
                fflush(stderr);
                last_dot = now;
            }

            /* seconds == 0 asks for exactly one attempt. */
            if (seconds == 0 || now >= deadline) {
                break;
            }

            continue;
        }

        /*
         * Anything else is a refusal rather than an empty frame, and repeating
         * it for the whole window would only bury the first errno.  Report and
         * stop: ERR_BUFFER_TOO_SMALL belongs here too, because it means the
         * capture path worked and the payload was simply too long.
         */
        if (attempts > 1 && last_dot != start) {
            fputc('\n', stderr);
        }

        if (status == UF_QR_CODE_STATUS_ERR_DEVICE) {
            fprintf(stderr, "probe: %s: %s (errno %d)\n", device, strerror(cause), cause);
            fprintf(stderr, "probe: is this a capture device? try: v4l2-ctl --list-devices\n");
            return PROBE_EXIT_DEVICE;
        }

        sReportStatus(status);

        return (status == UF_QR_CODE_STATUS_ERR_BUFFER_TOO_SMALL) ? PROBE_EXIT_READ : PROBE_EXIT_DEVICE;
    }

    if (attempts > 1 && last_dot != start) {
        fputc('\n', stderr);
    }

    fprintf(stderr, "probe: %u frame%s read from %s, no code in any of them\n",
            without_code, without_code == 1 ? "" : "s", device);
    fprintf(stderr, "probe: the camera works — hold the code closer, flatter, and steadier,"
                    " and check the light border around it is not clipped\n");

    return PROBE_EXIT_NO_CODE;
}
