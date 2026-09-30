/**
 * @file qr_code_frame_sender.c
 * @brief Send synthetic greyscale frames carrying a QR code to qr_code_socket_probe.
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
 * The other end of the socket, for hosts with no camera to point anywhere.
 *
 * It exists so the socket path can be exercised end to end without a camera and
 * without Windows: qr_code_socket_probe receiving from this program proves the
 * wire format, the framing, the read-exact loop and the decode hand-off all
 * work.  What it cannot prove is anything about optics — it renders a perfect
 * symbol from the encoder, which is precisely what the round-trip gtest suite
 * already does in-process.  A real camera is needed for the rest.
 *
 * frame_sender.py is this program's counterpart on a Windows host, and speaks
 * the same format; this one is the reference for it.
 *
 * The first frame is deliberately blank.  A scanner has to treat "no code in
 * this frame" as an ordinary event and keep going, and that is easy to get
 * wrong in a way that only shows up against a live camera — a wall, a hand, a
 * pause between codes.  Sending one blank frame first means the consumer's
 * positive-status path is covered by every run rather than only in theory.
 *
 * Build:  cmake -DUFLIB_CAPABILITY_QUIRC=ON -DUFLIB_CAPABILITY_QRENCODE=ON …
 *         cmake --build <dir> --target qr_code_frame_sender
 * Run:    ./qr_code_frame_sender [host] [port] [payload] [frames] [scale]
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/qr_code/utils_qr_code.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SENDER_DEFAULT_HOST     "127.0.0.1"
#define SENDER_DEFAULT_PORT     8080
#define SENDER_DEFAULT_PAYLOAD  "https://unfacd.io"
#define SENDER_DEFAULT_FRAMES   10
#define SENDER_DEFAULT_SCALE    4
#define SENDER_QUIET_MODULES    4      /* the light border the spec requires */

#define SENDER_HEADER_BYTES     16
#define SENDER_FRAME_MAGIC      0x5146524Du    /* 'QFRM', as the probe expects */

#define SENDER_CONNECT_ATTEMPTS 20
#define SENDER_CONNECT_DELAY_MS 100

static volatile sig_atomic_t s_interrupted = 0;

static void
sOnSignal(int signal_number)
{
    (void)signal_number;
    s_interrupted = 1;
}

static void
sSleepMs(unsigned milliseconds)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(milliseconds / 1000u),
        .tv_nsec = (long)(milliseconds % 1000u) * 1000000L,
    };

    nanosleep(&ts, NULL);
}

static void
sWriteBE32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static int
sWriteAll(int fd, const uint8_t *buffer, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t wrote = send(fd, buffer + sent, length - sent, 0);

        if (wrote > 0) {
            sent += (size_t)wrote;
            continue;
        }

        if (wrote < 0 && errno == EINTR) {
            if (s_interrupted) {
                return -1;
            }
            continue;
        }

        fprintf(stderr, "sender: send: %s (errno %d)\n", strerror(errno), errno);
        return -1;
    }

    return 0;
}

static int
sConnectWithRetry(const char *host, uint16_t port)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };

    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        fprintf(stderr, "sender: %s is not an IPv4 address\n", host);
        return -1;
    }

    for (int attempt = 1; attempt <= SENDER_CONNECT_ATTEMPTS && !s_interrupted; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            fprintf(stderr, "sender: socket: %s (errno %d)\n", strerror(errno), errno);
            return -1;
        }

        if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0) {
            return fd;
        }

        int cause = errno;

        close(fd);

        /*
         * The probe is normally started first and is listening, but a script
         * that starts both should not depend on winning that race.
         */
        if (cause != ECONNREFUSED && cause != ENOENT && cause != ETIMEDOUT) {
            fprintf(stderr, "sender: connect %s:%u: %s (errno %d)\n", host, port, strerror(cause), cause);
            return -1;
        }

        if (attempt == 1) {
            fprintf(stderr, "sender: waiting for the probe to listen on %s:%u\n", host, port);
        }

        sSleepMs(SENDER_CONNECT_DELAY_MS);
    }

    fprintf(stderr, "sender: gave up connecting to %s:%u\n", host, port);
    return -1;
}

/*
 * Lay the symbol out as a luminance raster with the quiet zone added around it,
 * which the encoder does not provide and every scanner needs.
 */
static void
sRasterise(const UfQrCodeMatrix *matrix, unsigned scale, uint8_t *image, uint32_t pitch)
{
    uint32_t span = (uint32_t)matrix->width + 2u * SENDER_QUIET_MODULES;

    for (uint32_t y = 0; y < span * scale; y++) {
        for (uint32_t x = 0; x < span * scale; x++) {
            uint32_t module_x = x / scale;
            uint32_t module_y = y / scale;

            uint8_t value = 0xff;   /* light, which the quiet zone always is */

            if (module_x >= SENDER_QUIET_MODULES && module_y >= SENDER_QUIET_MODULES) {
                size_t symbol_x = module_x - SENDER_QUIET_MODULES;
                size_t symbol_y = module_y - SENDER_QUIET_MODULES;

                if (symbol_x < matrix->width && symbol_y < matrix->width) {
                    value = matrix->modules[symbol_y * matrix->width + symbol_x] ? 0x00 : 0xff;
                }
            }

            image[(size_t)y * pitch + x] = value;
        }
    }
}

int
main(int argc, char **argv)
{
    const char *host    = (argc > 1) ? argv[1] : SENDER_DEFAULT_HOST;
    uint16_t    port    = SENDER_DEFAULT_PORT;
    const char *payload = (argc > 3) ? argv[3] : SENDER_DEFAULT_PAYLOAD;
    int         frames  = (argc > 4) ? atoi(argv[4]) : SENDER_DEFAULT_FRAMES;
    unsigned    scale   = (argc > 5) ? (unsigned)atoi(argv[5]) : SENDER_DEFAULT_SCALE;

    if (argc > 6) {
        fprintf(stderr, "usage: %s [host] [port] [payload] [frames] [scale]\n"
                        "       frames 0 runs until interrupted\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc > 2) {
        long value = strtol(argv[2], NULL, 10);

        if (value <= 0 || value > 65535) {
            fprintf(stderr, "sender: %s is not a port number\n", argv[2]);
            return EXIT_FAILURE;
        }

        port = (uint16_t)value;
    }

    if (frames < 0 || scale == 0 || scale > 64) {
        fprintf(stderr, "sender: frames must be >= 0 and scale between 1 and 64\n");
        return EXIT_FAILURE;
    }

    struct sigaction action = {0};
    action.sa_handler = sOnSignal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    UfQrCodeMatrix *matrix = NULL;
    UfQrCodeStatus status = UfUtilsQrCodeEncodeText(payload, &matrix);

    if (status != UF_QR_CODE_STATUS_OK) {
        fprintf(stderr, "sender: could not encode the payload, status %d\n", (int)status);
        return EXIT_FAILURE;
    }

    uint32_t span  = (uint32_t)matrix->width + 2u * SENDER_QUIET_MODULES;
    uint32_t pitch = span * scale;

    fprintf(stderr, "sender: symbol %zux%zu modules, %u bytes of payload,"
                    " rendering %ux%u at scale %u\n",
            matrix->width, matrix->width, (unsigned)strlen(payload), pitch, pitch, scale);

    uint8_t *image = malloc((size_t)pitch * pitch);

    if (image == NULL) {
        fprintf(stderr, "sender: out of memory for a %ux%u frame\n", pitch, pitch);
        free(matrix);
        return EXIT_FAILURE;
    }

    sRasterise(matrix, scale, image, pitch);
    free(matrix);

    int connection = sConnectWithRetry(host, port);

    if (connection < 0) {
        free(image);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "sender: connected to %s:%u\n", host, port);

    uint8_t *blank = malloc((size_t)pitch * pitch);

    if (blank == NULL) {
        fprintf(stderr, "sender: out of memory for the blank frame\n");
        free(image);
        close(connection);
        return EXIT_FAILURE;
    }

    memset(blank, 0xff, (size_t)pitch * pitch);

    uint32_t header[SENDER_HEADER_BYTES / 4];

    unsigned sent = 0;
    int exit_code = EXIT_SUCCESS;

    /*
     * One blank frame first: the consumer must read it, find nothing, and carry
     * on rather than treating an empty frame as the end of anything.
     */
    for (unsigned index = 0; ; index++) {
        bool is_blank = (index == 0);
        const uint8_t *frame = is_blank ? blank : image;

        sWriteBE32((uint8_t *)header,      SENDER_FRAME_MAGIC);
        sWriteBE32((uint8_t *)header + 4,  pitch);
        sWriteBE32((uint8_t *)header + 8,  pitch);
        sWriteBE32((uint8_t *)header + 12, pitch * pitch);

        if (sWriteAll(connection, (const uint8_t *)header, sizeof header) != 0 ||
            sWriteAll(connection, frame, (size_t)pitch * pitch) != 0) {
            exit_code = EXIT_FAILURE;
            break;
        }

        sent++;

        if (is_blank) {
            fprintf(stderr, "sender: frame %u blank, by design\n", sent);
        } else {
            fprintf(stderr, "sender: frame %u carries the code\n", sent);
        }

        /* frames counts coded frames; the blank one is always sent first. */
        if (frames > 0 && index >= (unsigned)frames) {
            break;
        }

        if (s_interrupted) {
            break;
        }

        sSleepMs(100);
    }

    free(blank);
    free(image);
    close(connection);

    fprintf(stderr, "sender: %u frame%s sent\n", sent, sent == 1 ? "" : "s");

    return exit_code;
}
