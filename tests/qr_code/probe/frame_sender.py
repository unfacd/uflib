#!/usr/bin/env python3
"""
frame_sender.py -- capture a webcam and stream greyscale frames to
qr_code_socket_probe running inside WSL2.

This is the Windows end of the socket transport.  It exists because a Windows
webcam is not a Linux device: WSL2 has no /dev/video* until something attaches
the hardware, and this sidesteps that by never letting Linux see the camera at
all.  Windows reads the frames, WSL2 receives them, and the library decodes
them -- no kernel work, no USB passthrough.

The wire format is the one qr_code_socket_probe documents and
qr_code_frame_sender.c implements:

    uint32 big-endian   magic, 'QFRM'
    uint32 big-endian   width
    uint32 big-endian   height
    uint32 big-endian   payload bytes, equal to width*height
    uint8[]             payload, row-major, stride == width

Setup on Windows:

    pip install opencv-python

Then, with the probe already listening:

    python frame_sender.py <wsl-ip> 8080

The address is the one qr_code_socket_probe printed as "point the sender at".
It is NOT 127.0.0.1: that is the Windows loopback, while the listener is in a
separate network namespace inside WSL2.  Under WSL2's mirrored networking mode
localhost does work, but the printed address works under both, so prefer it.

Nothing here knows what a QR code is.  It sends luminance and stops.
"""

import argparse
import socket
import struct
import sys
import time

try:
    import cv2
except ImportError:
    sys.exit("frame_sender: opencv is missing -- run: pip install opencv-python")

# 'QFRM' -- the same integer the C side packs, so both read the same bytes.
FRAME_MAGIC = 0x5146524D

HEADER = struct.Struct(">IIII")


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Stream webcam frames to qr_code_socket_probe."
    )
    parser.add_argument("host", help="address the probe printed, e.g. 172.29.54.7")
    parser.add_argument("port", type=int, nargs="?", default=8080)
    parser.add_argument("--camera", type=int, default=0, help="camera index, default 0")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument(
        "--fps",
        type=float,
        default=10.0,
        help="frames per second to aim for, default 10; the probe's backpressure "
        "ultimately sets the pace",
    )
    return parser.parse_args()


def connect(host, port, attempts=20, delay=0.5):
    """
    Retry while the probe comes up.  Starting both ends from one script should
    not depend on winning a race.
    """
    for attempt in range(1, attempts + 1):
        try:
            connection = socket.create_connection((host, port), timeout=5.0)
        except OSError as error:
            if attempt == 1:
                print(f"frame_sender: waiting for the probe on {host}:{port}", file=sys.stderr)
            if attempt == attempts:
                sys.exit(f"frame_sender: gave up connecting to {host}:{port}: {error}")
            time.sleep(delay)
            continue

        # A stalled consumer must not wedge the capture loop.
        connection.settimeout(10.0)
        return connection

    sys.exit("frame_sender: unreachable")


def main():
    arguments = parse_arguments()

    capture = cv2.VideoCapture(arguments.camera, cv2.CAP_DSHOW)

    if not capture.isOpened():
        # CAP_DSHOW is the Windows backend and the fastest to open; fall back to
        # the default one rather than failing on a platform that lacks it.
        capture = cv2.VideoCapture(arguments.camera)

    if not capture.isOpened():
        sys.exit(f"frame_sender: could not open camera {arguments.camera}")

    capture.set(cv2.CAP_PROP_FRAME_WIDTH, arguments.width)
    capture.set(cv2.CAP_PROP_FRAME_HEIGHT, arguments.height)

    connection = connect(arguments.host, arguments.port)

    print(f"frame_sender: streaming to {arguments.host}:{arguments.port}", file=sys.stderr)

    interval = 1.0 / arguments.fps if arguments.fps > 0 else 0.0
    sent = 0
    announced = None

    try:
        while True:
            started = time.monotonic()

            ok, frame = capture.read()

            if not ok:
                print("frame_sender: camera returned no frame", file=sys.stderr)
                break

            grey = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

            # A driver may substitute a resolution for the one requested, and
            # may do it again later.  The header carries whatever was actually
            # captured, so a change is reported rather than silently corrupting
            # the pitch the far end decodes at.
            height, width = grey.shape[:2]

            if announced != (width, height):
                print(
                    f"frame_sender: capturing {width}x{height}"
                    f" (asked for {arguments.width}x{arguments.height})",
                    file=sys.stderr,
                )
                announced = (width, height)

            payload = grey.tobytes()

            connection.sendall(HEADER.pack(FRAME_MAGIC, width, height, len(payload)))
            connection.sendall(payload)

            sent += 1

            remaining = interval - (time.monotonic() - started)
            if remaining > 0:
                time.sleep(remaining)

    except KeyboardInterrupt:
        pass
    except (BrokenPipeError, ConnectionResetError) as error:
        print(f"frame_sender: the probe went away: {error}", file=sys.stderr)
    finally:
        capture.release()
        connection.close()
        print(f"frame_sender: {sent} frames sent", file=sys.stderr)


if __name__ == "__main__":
    main()
