#
# tests/qr_code/probe/CMakeLists.txt — a camera-reading console program
#
# A consumer, not a test: no add_test() below.  A person runs this with a
# printed code in hand; it has no business in a CI gate, and it cannot pass one
# on a box without a camera.
#
# Gated on UFLIB_CAPABILITY_QUIRC alone, and that is the point rather than an
# oversight: the probe reads a camera and prints bytes, so it needs no encoder.
# Building it without libqrencode present is the standing proof that the two
# halves of the module are separable — with the encoder absent this target
# still configures, compiles and links.
#

if(NOT UFLIB_CAPABILITY_QUIRC)
    return()
endif()

add_executable(qr_code_camera_probe qr_code_camera_probe.c)

target_include_directories(qr_code_camera_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)

# libm arrives through quirc's own INTERFACE; libqrencode is deliberately absent.
target_link_libraries(qr_code_camera_probe PRIVATE uflib::uflib)

ufsrv_apply_build_options(qr_code_camera_probe)

message(STATUS "QR-code module: camera probe built — qr_code_camera_probe")

# ── Socket probe — frames from a TCP peer instead of a V4L2 device ────────
# For a host whose camera is not reachable as a Linux device, WSL2 with a
# Windows webcam being the case it was written for.  Still quirc only: it
# decodes, and the transport is its own business.

add_executable(qr_code_socket_probe qr_code_socket_probe.c)

target_include_directories(qr_code_socket_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)

target_link_libraries(qr_code_socket_probe PRIVATE uflib::uflib)

ufsrv_apply_build_options(qr_code_socket_probe)

message(STATUS "QR-code module: socket probe built — qr_code_socket_probe")

# ── Frame sender — the socket probe's other end, for hosts with no camera ──
# Needs the encoder as well, so it is gated on both capabilities.  It is a
# verification tool rather than a consumer of the decode path, which is why it
# is the one target here that names libqrencode.

if(UFLIB_CAPABILITY_QRENCODE)
    add_executable(qr_code_frame_sender qr_code_frame_sender.c)

    target_include_directories(qr_code_frame_sender PRIVATE
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}
    )

    target_link_libraries(qr_code_frame_sender PRIVATE
        uflib::uflib
        ${QRENCODE_LIBRARIES}
    )

    ufsrv_apply_build_options(qr_code_frame_sender)

    message(STATUS "QR-code module: frame sender built — qr_code_frame_sender")
endif()
