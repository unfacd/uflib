/**
 * @file qr_code_camera_demo.c
 * @brief An ncurses front end for the QR-code module: point a camera at a code,
 *        read it, and see it rendered back.
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
 * This is a consumer of the library, not part of it, and it is deliberately the
 * smallest one that is still useful.  Everything that was reusable in the
 * program it came from now lives in uflib: this file holds a terminal, an event
 * loop, and three calls.
 *
 * There is no V4L2 here at all — no ioctl, no mmap, no buffer queue, no YUYV
 * conversion, no quirc handle, no pixel format.  The device is a path, and the
 * only thing this file knows about a camera is where it usually lives.  That is
 * the measure of whether the extraction worked: if this file needed a device
 * opened, it did not.
 *
 * The one thing worth understanding before reading further is why the capture
 * call is inside the redraw rather than in a loop of its own.  The public
 * surface has no capture handle — a call takes a device path and returns text —
 * so a live preview means one open/capture/close per displayed frame.  That is
 * a few ioctls and two mappings every redraw interval, which at a hundred
 * milliseconds is nothing, and it is the price of keeping the API at three
 * symbols.  A program that needed to run a camera flat out would want the
 * handle API, and does not exist yet.
 *
 * Build:  cmake -DUFLIB_CAPABILITY_QUIRC=ON -DUFLIB_CAPABILITY_QRENCODE=ON …
 * Run:    ./qr_code_camera_demo [/dev/video0]
 */

#ifdef HAVE_CONFIG_UFLIB_H
#include <config_uflib.h>
#endif

#include <uflib/qr_code/utils_qr_code.h>

#include <errno.h>
#include <locale.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_CAPTURE_WIDTH   640
#define DEMO_CAPTURE_HEIGHT  480
#define DEMO_REDRAW_MS       100
#define DEMO_TEXT_CAPACITY   512
#define DEMO_QUIET_ZONE      4      /* modules of light border the specification requires */

/*
 * One character cell holds one module across and two down, drawn with the
 * half-block glyphs.  A terminal cell is about twice as tall as it is wide, so
 * this is what makes the rendered symbol square on screen — and a symbol that
 * is not square is one a phone camera has to work to read.
 */
typedef struct DemoRenderer {
    WINDOW *window;
    int     rows;
    int     cols;
} DemoRenderer;

static void
sDemoDrawModulePair(WINDOW *window, int row, int col, uint8_t upper, uint8_t lower)
{
    /* U+2580 upper half, U+2584 lower half, U+2588 full block. */
    if (upper && lower) {
        mvwaddstr(window, row, col, "█");
    } else if (upper) {
        mvwaddstr(window, row, col, "▀");
    } else if (lower) {
        mvwaddstr(window, row, col, "▄");
    } else {
        mvwaddstr(window, row, col, " ");
    }
}

/*
 * Render the matrix, scaled to whole modules, centred, clipped to the window.
 *
 * The scale is derived from the window rather than fixed, because the symbol
 * size is the encoder's choice: a short payload yields a 21-module symbol and a
 * long one a 177-module symbol, and a fixed scale would show one of them as a
 * speck and the other as a crop.
 *
 * The quiet zone is added here rather than by the library, which returns the
 * data region only.  Doing it at the point of rendering is what the library's
 * contract asks for, and it is the reason a freshly rendered code can be
 * scanned at all.
 */
static void
sDemoRenderMatrix(const UfQrCodeMatrix *matrix, DemoRenderer *renderer)
{
    if (matrix == NULL || renderer->rows <= 0 || renderer->cols <= 0) {
        return;
    }

    /* Each cell carries two modules vertically, so the usable height in modules
       is twice the row count. */
    int across = (int)matrix->width + 2 * DEMO_QUIET_ZONE;
    int down   = across;

    int scale_x = renderer->cols / across;
    int scale_y = (renderer->rows * 2) / down;

    int scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale < 1) {
        scale = 1;   /* too small to show whole: clip rather than vanish */
    }

    int drawn_cols = across * scale;
    int drawn_rows = (down * scale + 1) / 2;   /* cells, rounding the odd module up */

    int origin_col = (renderer->cols - drawn_cols) / 2;
    int origin_row = (renderer->rows - drawn_rows) / 2;

    if (origin_col < 0) {
        origin_col = 0;
    }
    if (origin_row < 0) {
        origin_row = 0;
    }

    for (int cell_row = 0; cell_row < drawn_rows && origin_row + cell_row < renderer->rows; cell_row++) {
        for (int cell_col = 0; cell_col < drawn_cols && origin_col + cell_col < renderer->cols; cell_col++) {

            /* Map the cell back to the two modules it covers, in symbol
               coordinates with the quiet zone removed. */
            int upper_x = cell_col / scale - DEMO_QUIET_ZONE;
            int upper_y = (cell_row * 2)     / scale - DEMO_QUIET_ZONE;
            int lower_y = (cell_row * 2 + 1) / scale - DEMO_QUIET_ZONE;

            uint8_t upper = 0;
            uint8_t lower = 0;

            if (upper_x >= 0 && (size_t)upper_x < matrix->width) {
                if (upper_y >= 0 && (size_t)upper_y < matrix->width) {
                    upper = matrix->modules[(size_t)upper_y * matrix->width + (size_t)upper_x];
                }
                if (lower_y >= 0 && (size_t)lower_y < matrix->width) {
                    lower = matrix->modules[(size_t)lower_y * matrix->width + (size_t)upper_x];
                }
            }

            sDemoDrawModulePair(renderer->window, origin_row + cell_row, origin_col + cell_col, upper, lower);
        }
    }
}

static void
sDemoDrawStatus(WINDOW *window, const char *device, UfQrCodeStatus status, int cause, const char *payload)
{
    werase(window);
    box(window, 0, 0);
    mvwprintw(window, 0, 2, " qr_code — %s ", device);

    switch (status) {
        case UF_QR_CODE_STATUS_OK:
            mvwprintw(window, 1, 2, "code: %s", payload);
            break;

        case UF_QR_CODE_STATUS_NO_CODE_FOUND:
            /* Positive on purpose: the frame was read and held no code.  This is
               the steady state of a camera pointed at a wall, and it is not an
               error — which is why it is drawn as though nothing were wrong. */
            mvwprintw(window, 1, 2, "no code in view");
            break;

        case UF_QR_CODE_STATUS_ERR_INVALID_ARGUMENT:
            mvwprintw(window, 1, 2, "invalid argument");
            break;

        case UF_QR_CODE_STATUS_ERR_BUFFER_TOO_SMALL:
            mvwprintw(window, 1, 2, "payload does not fit the buffer");
            break;

        case UF_QR_CODE_STATUS_ERR_NOMEM:
            mvwprintw(window, 1, 2, "out of memory");
            break;

        case UF_QR_CODE_STATUS_ERR_ENCODE:
            mvwprintw(window, 1, 2, "the encoder refused the payload");
            break;

        case UF_QR_CODE_STATUS_ERR_ENCODE_TOO_LARGE:
            mvwprintw(window, 1, 2, "payload needs a symbol larger than the specification allows");
            break;

        case UF_QR_CODE_STATUS_ERR_DEVICE:
            /* errno carries the cause for this one status and no other, so it is
               printed here and nowhere else. */
            mvwprintw(window, 1, 2, "device error: %s", strerror(cause));
            break;

        case UF_QR_CODE_STATUS_ERR_UNSUPPORTED:
            mvwprintw(window, 1, 2, "capture is not supported on this platform");
            break;

        default:
            mvwprintw(window, 1, 2, "unexpected status %d", (int)status);
            break;
    }

    mvwprintw(window, 2, 2, "q quits");
    wrefresh(window);
}

int
main(int argc, char **argv)
{
    const char *device = (argc > 1) ? argv[1] : "/dev/video0";

    /* Before initscr(), so the half-block glyphs are written as the UTF-8 the
       terminal is expecting rather than as whatever the C locale implies. */
    setlocale(LC_ALL, "");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(DEMO_REDRAW_MS);

    if (!has_colors()) {
        endwin();
        fprintf(stderr, "qr_code_camera_demo: this terminal has no colour support\n");
        return EXIT_FAILURE;
    }

    start_color();
    use_default_colors();
    init_pair(1, COLOR_BLACK, COLOR_WHITE);

    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);

    const int status_rows = 4;

    WINDOW *status_window = newwin(status_rows, cols, 0, 0);
    WINDOW *code_window   = newwin(rows - status_rows, cols, status_rows, 0);

    if (status_window == NULL || code_window == NULL) {
        endwin();
        fprintf(stderr, "qr_code_camera_demo: could not create the windows\n");
        return EXIT_FAILURE;
    }

    wbkgd(code_window, COLOR_PAIR(1));

    DemoRenderer renderer = {
        .window = code_window,
        .rows   = rows - status_rows - 2,   /* inside the border */
        .cols   = cols - 2,
    };

    char   text[DEMO_TEXT_CAPACITY];
    size_t length = 0;

    /*
     * The payload is held across frames so that a code leaving the frame does
     * not blank the display.  Nothing writes this buffer unless a code was
     * read, which is the library's documented behaviour and the reason the
     * carry-over is correct rather than a lucky accident.
     */
    char   last_payload[DEMO_TEXT_CAPACITY] = {0};
    bool   have_payload = false;

    UfQrCodeStatus status = UF_QR_CODE_STATUS_NO_CODE_FOUND;
    int            cause  = 0;

    bool running = true;

    while (running) {
        /* One frame per redraw.  A timeout of zero asks for a single capture
           attempt and no waiting, which is what keeps the interface live. */
        status = UfUtilsQrCodeCameraGenerate(device, DEMO_CAPTURE_WIDTH, DEMO_CAPTURE_HEIGHT, 0,
                                             text, sizeof text, &length);
        cause = errno;

        if (status == UF_QR_CODE_STATUS_OK) {
            memcpy(last_payload, text, length + 1);
            have_payload = true;
        }

        sDemoDrawStatus(status_window, device, status, cause, last_payload);

        werase(code_window);
        box(code_window, 0, 0);

        if (have_payload) {
            UfQrCodeMatrix *matrix = NULL;

            if (UfUtilsQrCodeEncodeText(last_payload, &matrix) == UF_QR_CODE_STATUS_OK) {
                sDemoRenderMatrix(matrix, &renderer);
                free(matrix);   /* one allocation, one free — the module's contract */
            } else {
                mvwprintw(code_window, 1, 2, "the payload could not be re-encoded");
            }
        } else {
            mvwprintw(code_window, 1, 2, "nothing scanned yet");
        }

        wrefresh(code_window);

        int key = getch();

        if (key == 'q' || key == 'Q') {
            running = false;
        }
    }

    delwin(status_window);
    delwin(code_window);
    endwin();

    return EXIT_SUCCESS;
}
