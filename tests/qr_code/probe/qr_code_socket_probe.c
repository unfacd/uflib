/**
 * @file qr_code_socket_probe.c
 * @brief Read greyscale frames from a TCP socket and print the QR code in them.
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
 * The alternative source of frames, for a host whose camera is not reachable
 * as a V4L2 device — a Windows webcam behind WSL2 being the case this exists
 * for.  Something outside the machine captures and reduces the picture to
 * luminance; this program receives it and hands it to UfUtilsQrCodeDecode().
 *
 * Nothing in the library is bypassed or special-cased.  UfUtilsQrCodeDecode()
 * is documented as taking one 8-bit luminance raster and a stride, and a socket
 * is just another producer of that — the module never learns a socket is
 * involved.  That is the entire reason this needs no change to uflib.
 *
 * The wire format is deliberately self-describing rather than assumed:
 *
 *     uint32 big-endian   magic, 'QFRM'
 *     uint32 big-endian   width
 *     uint32 big-endian   height
 *     uint32 big-endian   payload bytes, which must equal width*height
 *     uint8[]             payload, row-major, stride == width
 *
 * Sixteen bytes of header buy two things worth more than the four bytes a bare
 * length prefix would cost.  Geometry that disagrees with the consumer is
 * refused by name instead of producing a frame decoded at the wrong pitch, and
 * a sender may change resolution mid-stream — a camera renegotiating does this
 * without asking.
 *
 * On pacing: this reads a frame, decodes it, then reads the next, and TCP
 * backpressure means the sender blocks rather than running ahead.  The backlog
 * is therefore whatever the socket buffer holds, capped by the kernel, rather
 * than growing without limit — the failure the naive `sendall` loop has.  Every
 * frame is examined and none is dropped, which is what a scanner wants; a tool
 * that wanted a live preview would instead drain to the newest frame and lose
 * the ones in between.
 *
 * Build:  cmake -DUFLIB_CAPABILITY_QUIRC=ON -D_PACKAGE_TESTS=ON …
 *         cmake --build <dir> --target qr_code_socket_probe
 * Run:    ./qr_code_socket_probe [port] [seconds]
 *
 * The peer is tests/qr_code/probe/frame_sender.py on a Windows host, or
 * qr_code_frame_sender on this one — both speak the format above.
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/qr_code/utils_qr_code.h>

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PROBE_DEFAULT_PORT      8080
#define PROBE_HEADER_BYTES      16
#define PROBE_FRAME_MAGIC       0x5146524Du    /* 'QFRM' */

/* A refusal, not a crash, if a peer announces a frame larger than any camera
   has business sending — 8192 square is 64 MiB and well past any sensor. */
#define PROBE_MAX_DIMENSION     8192u
#define PROBE_MAX_PAYLOAD       ((size_t)PROBE_MAX_DIMENSION * PROBE_MAX_DIMENSION)

#define PROBE_TEXT_CAPACITY     4096
#define PROBE_READ_TIMEOUT_S    5

static volatile sig_atomic_t s_interrupted = 0;

static void
sOnSignal(int signal_number)
{
    (void)signal_number;
    s_interrupted = 1;
}

static uint64_t
sNowMs(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint32_t
sReadBE32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8)  |  (uint32_t)bytes[3];
}

/*
 * Read exactly length bytes.  A socket is a byte stream with no message
 * boundaries, so a single recv() returning less than asked is normal and legal
 * — treating a short read as a whole frame is the classic way to lose sync with
 * the sender on the first frame that arrives split across two packets.
 *
 * @p label names what was being read.  A sender that leaves between frames and
 * one that dies halfway through one produce the same EOF, and telling those
 * apart is the difference between "the capture stopped" and "the transport is
 * truncating", which are not the same problem.
 */
static int
sReadExact(int fd, uint8_t *buffer, size_t length, const char *label)
{
    size_t received = 0;

    while (received < length) {
        ssize_t got = recv(fd, buffer + received, length - received, 0);

        if (got > 0) {
            received += (size_t)got;
            continue;
        }

        if (got == 0) {
            if (received == 0) {
                fprintf(stderr, "probe: the sender closed the connection between frames\n");
            } else {
                fprintf(stderr, "probe: the sender left partway through the %s:"
                                " %zu of %zu bytes\n", label, received, length);
            }
            return -1;
        }

        if (errno == EINTR) {
            if (s_interrupted) {
                return -1;
            }
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            fprintf(stderr, "probe: the sender stalled partway through the %s: %zu of %zu bytes\n",
                    label, received, length);
            return -1;
        }

        fprintf(stderr, "probe: recv: %s (errno %d)\n", strerror(errno), errno);
        return -1;
    }

    return 0;
}

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

/*
 * Interfaces a sender must never be pointed at.  Loopback is the peer's own
 * machine, and the container bridges are local plumbing — a host that has
 * docker running otherwise prints seven addresses of which one is useful, and a
 * list that long is one people stop reading.
 */
static bool
sIsUnreachableInterface(const char *name)
{
    static const char *const prefixes[] = { "docker", "br-", "veth", "virbr", "tun", "tap" };

    if (strcmp(name, "lo") == 0) {
        return true;
    }

    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * Print the addresses the peer could reach this listener on.  A Windows sender
 * cannot use 127.0.0.1 for a WSL2 listener — that is its own loopback, not this
 * machine's — so the address has to be read off this end rather than guessed at
 * the other one.  It also changes across reboots, which is why it is printed
 * per run instead of documented once.
 */
static void
sPrintListenAddresses(uint16_t port)
{
    struct ifaddrs *interfaces = NULL;
    unsigned printed = 0;

    if (getifaddrs(&interfaces) != 0) {
        fprintf(stderr, "probe: could not enumerate interfaces; connect to this host's"
                        " non-loopback address on port %u\n", port);
        return;
    }

    for (struct ifaddrs *entry = interfaces; entry != NULL; entry = entry->ifa_next) {
        if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if (entry->ifa_name == NULL || sIsUnreachableInterface(entry->ifa_name)) {
            continue;
        }

        char text[INET_ADDRSTRLEN] = {0};
        const struct sockaddr_in *address = (const struct sockaddr_in *)entry->ifa_addr;

        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof text) == NULL) {
            continue;
        }

        fprintf(stderr, "probe:   point the sender at %s:%u\n", text, port);
        printed++;
    }

    /* Silence here would be indistinguishable from not having tried, and the
       sender genuinely has nowhere to connect to in that case. */
    if (printed == 0) {
        fprintf(stderr, "probe:   this host has no address a peer could reach on port %u\n", port);
    }

    freeifaddrs(interfaces);
}

static int
sCreateListener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        fprintf(stderr, "probe: socket: %s (errno %d)\n", strerror(errno), errno);
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) != 0) {
        fprintf(stderr, "probe: SO_REUSEADDR: %s (errno %d)\n", strerror(errno), errno);
    }

    struct sockaddr_in address = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(port),
    };

    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        fprintf(stderr, "probe: bind port %u: %s (errno %d)\n", port, strerror(errno), errno);
        close(fd);
        return -1;
    }

    if (listen(fd, 4) != 0) {
        fprintf(stderr, "probe: listen: %s (errno %d)\n", strerror(errno), errno);
        close(fd);
        return -1;
    }

    return fd;
}

static int
sAcceptWithin(int listener, uint64_t deadline_ms, bool have_deadline)
{
    for (;;) {
        if (s_interrupted) {
            return -1;
        }

        int wait_ms = -1;

        if (have_deadline) {
            uint64_t now = sNowMs();

            if (now >= deadline_ms) {
                fprintf(stderr, "probe: no sender connected in time\n");
                return -1;
            }

            wait_ms = (int)(deadline_ms - now);
        }

        struct pollfd entry = { .fd = listener, .events = POLLIN, .revents = 0 };
        int ready = poll(&entry, 1, wait_ms);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "probe: poll: %s (errno %d)\n", strerror(errno), errno);
            return -1;
        }

        if (ready == 0) {
            fprintf(stderr, "probe: no sender connected in time\n");
            return -1;
        }

        int connection = accept(listener, NULL, NULL);

        if (connection < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "probe: accept: %s (errno %d)\n", strerror(errno), errno);
            return -1;
        }

        return connection;
    }
}

int
main(int argc, char **argv)
{
    uint16_t port = PROBE_DEFAULT_PORT;
    int seconds = 0;

    if (argc > 3) {
        fprintf(stderr, "usage: %s [port] [seconds]\n"
                        "       seconds 0 (default) runs until interrupted or the sender leaves\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (argc > 1) {
        long value = strtol(argv[1], NULL, 10);

        if (value <= 0 || value > 65535) {
            fprintf(stderr, "probe: %s is not a port number\n", argv[1]);
            return EXIT_FAILURE;
        }

        port = (uint16_t)value;
    }

    if (argc > 2) {
        seconds = atoi(argv[2]);

        if (seconds < 0) {
            fprintf(stderr, "probe: %s is not a number of seconds\n", argv[2]);
            return EXIT_FAILURE;
        }
    }

    /*
     * Ctrl-C is how a live scanning tool is stopped, so it should report its
     * tally on the way out rather than dying at the signal.
     */
    struct sigaction action = {0};
    action.sa_handler = sOnSignal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    int listener = sCreateListener(port);

    if (listener < 0) {
        return EXIT_FAILURE;
    }

    fprintf(stderr, "probe: listening on port %u\n", port);
    sPrintListenAddresses(port);

    bool have_deadline = seconds > 0;
    uint64_t deadline = sNowMs() + (uint64_t)seconds * 1000u;

    int connection = sAcceptWithin(listener, deadline, have_deadline);

    if (connection < 0) {
        close(listener);
        return EXIT_FAILURE;
    }

    /*
     * A stalled sender must not hang the tool forever, and the frame loop has
     * no other way to tell "slow" from "gone".
     */
    struct timeval timeout = { .tv_sec = PROBE_READ_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    fprintf(stderr, "probe: sender connected\n");

    uint8_t *frame = NULL;
    size_t   frame_capacity = 0;

    unsigned frames = 0;
    unsigned codes = 0;
    unsigned refusals = 0;

    char   text[PROBE_TEXT_CAPACITY];
    size_t length = 0;

    int exit_code = EXIT_SUCCESS;

    while (!s_interrupted) {
        uint8_t header[PROBE_HEADER_BYTES];

        if (sReadExact(connection, header, sizeof header, "frame header") != 0) {
            break;
        }

        uint32_t magic  = sReadBE32(header);
        uint32_t width  = sReadBE32(header + 4);
        uint32_t height = sReadBE32(header + 8);
        uint32_t payload = sReadBE32(header + 12);

        /*
         * Every one of these is fatal rather than skipped: a frame that does
         * not parse means the stream is no longer where this end thinks it is,
         * and reading on would decode noise with no way to notice.
         */
        if (magic != PROBE_FRAME_MAGIC) {
            fprintf(stderr, "probe: stream desynchronised — expected frame magic, got 0x%08x\n", magic);
            exit_code = EXIT_FAILURE;
            break;
        }

        if (width == 0 || height == 0 ||
            width > PROBE_MAX_DIMENSION || height > PROBE_MAX_DIMENSION) {
            fprintf(stderr, "probe: sender announced an unusable geometry, %ux%u\n", width, height);
            exit_code = EXIT_FAILURE;
            break;
        }

        if ((uint64_t)payload != (uint64_t)width * (uint64_t)height) {
            fprintf(stderr, "probe: sender announced %u bytes for %ux%u, which is %llu\n",
                    payload, width, height, (unsigned long long)((uint64_t)width * height));
            exit_code = EXIT_FAILURE;
            break;
        }

        if (payload > PROBE_MAX_PAYLOAD) {
            fprintf(stderr, "probe: sender announced an oversized frame, %u bytes\n", payload);
            exit_code = EXIT_FAILURE;
            break;
        }

        if (payload > frame_capacity) {
            uint8_t *grown = realloc(frame, payload);

            if (grown == NULL) {
                fprintf(stderr, "probe: out of memory for a %u byte frame\n", payload);
                exit_code = EXIT_FAILURE;
                break;
            }

            frame = grown;
            frame_capacity = payload;
        }

        if (sReadExact(connection, frame, payload, "frame payload") != 0) {
            break;
        }

        frames++;

        /* stride == width: the wire format is packed, with no row padding. */
        UfQrCodeStatus status = UfUtilsQrCodeDecode(frame, width, height, width,
                                                    text, sizeof text, &length);

        if (status == UF_QR_CODE_STATUS_OK) {
            codes++;
            printf("frame %u (%ux%u): length %zu\n", frames, width, height, length);
            printf("payload: ");
            sPrintEscaped(text, length);
            printf("\n");
            fflush(stdout);
            continue;
        }

        if (status == UF_QR_CODE_STATUS_NO_CODE_FOUND) {
            continue;
        }

        refusals++;
        fprintf(stderr, "probe: frame %u (%ux%u) refused with status %d\n",
                frames, width, height, (int)status);
    }

    if (frames > 0 && codes == 0 && refusals == 0) {
        fprintf(stderr, "probe: %u frames received and decoded with no code in any of them\n", frames);
        fprintf(stderr, "probe: the transport works — the picture is the problem:"
                        " focus, framing, glare, or the quiet zone clipped\n");
    } else {
        fprintf(stderr, "probe: %u frame%s, %u with a code, %u refused\n",
                frames, frames == 1 ? "" : "s", codes, refusals);
    }

    free(frame);
    close(connection);
    close(listener);

    return exit_code;
}
