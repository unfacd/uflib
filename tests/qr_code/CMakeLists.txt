#
# tests/qr_code/CMakeLists.txt — tests for the QR-code module
#
# The suites are gated the same way the module is, and for a reason that is not
# merely tidiness: with a capability off the symbols under test do not exist, so
# a test gated at run time would not link at all.  The gate is here, at build
# time, where the answer is known.
#
# Three suites rather than one, because they need different things:
#
#   decode     needs quirc and nothing else
#   camera     needs quirc; it exercises a device that is not a camera
#   encode     needs libqrencode, which uflib deliberately does not put on a
#              consumer's link line — so this target names it, exactly as a
#              consumer of UfUtilsQrCodeEncodeText would have to
#   roundtrip  needs both, since it is the two halves against each other
#
# libm is not named anywhere below: quirc declares `INTERFACE m` on itself and
# that reaches these targets through uflib::uflib.  add_c_test() also appends m
# unconditionally, which covers the encoder's target on its own.
#

set(_qr_test_include_dirs
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)

if(UFLIB_CAPABILITY_QUIRC)
    add_c_test(
        NAME utils_qr_code_decode_tests
        TEST_SOURCE utils_qr_code_decode_tests.cpp
        INCLUDE_DIRS ${_qr_test_include_dirs}
        LIBRARIES uflib::uflib pthread
    )

    add_c_test(
        NAME utils_qr_code_camera_tests
        TEST_SOURCE utils_qr_code_camera_tests.cpp
        INCLUDE_DIRS ${_qr_test_include_dirs}
        LIBRARIES uflib::uflib pthread
    )
endif()

if(UFLIB_CAPABILITY_QRENCODE)
    add_c_test(
        NAME utils_qr_code_encode_tests
        TEST_SOURCE utils_qr_code_encode_tests.cpp
        INCLUDE_DIRS ${_qr_test_include_dirs}
        LIBRARIES uflib::uflib pthread ${QRENCODE_LIBRARIES}
    )
endif()

if(UFLIB_CAPABILITY_QUIRC AND UFLIB_CAPABILITY_QRENCODE)
    add_c_test(
        NAME utils_qr_code_roundtrip_tests
        TEST_SOURCE utils_qr_code_roundtrip_tests.cpp
        INCLUDE_DIRS ${_qr_test_include_dirs}
        LIBRARIES uflib::uflib pthread ${QRENCODE_LIBRARIES}
    )
endif()

# ── Demo — the ncurses front end, as a consumer of the library ────────────
# A consumer, not a test: no add_test() anywhere below.  Built only when the
# module and ncurses are both available.

add_subdirectory(demo)

# ── Probe — a camera-reading console program, decode-only ─────────────────
# Also a consumer, also not a test.  Gated on quirc alone, so it builds on a
# host that has no encoder — which is what makes it the independent check on
# the camera path rather than a second view of the same round trip.

add_subdirectory(probe)
