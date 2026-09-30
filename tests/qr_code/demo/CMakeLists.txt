#
# tests/qr_code/demo/CMakeLists.txt — the ncurses front end, as a consumer
#
# A plain add_executable with no add_test() anywhere: this is a program a person
# runs, not a gate.  The pattern is tests/cdt/lockless_ringbuffer's standalone
# stress binary.
#
# Built only when the module has both halves — the demo decodes from a camera
# and renders what it read — and only when a curses library is present.  A
# missing curses is reported rather than silently skipped, so a build log says
# why the target is absent instead of leaving it to be inferred from silence.
#

if(NOT (UFLIB_CAPABILITY_QUIRC AND UFLIB_CAPABILITY_QRENCODE))
    return()
endif()

# Wide, because the renderer draws half-block glyphs (U+2580 / U+2584) to keep
# the symbol square on a terminal cell.  The narrow library would write those
# bytes without knowing they are multi-byte.
set(CURSES_NEED_WIDE TRUE)
set(CURSES_NEED_NCURSES TRUE)
find_package(Curses)

if(NOT CURSES_FOUND)
    message(STATUS "QR-code module: demo not built — no curses library found")
    return()
endif()

add_executable(qr_code_camera_demo qr_code_camera_demo.c)

target_include_directories(qr_code_camera_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
    ${CURSES_INCLUDE_DIRS}
)

# uflib::uflib carries libm through quirc's own INTERFACE; the encoder's
# libqrencode is named here, which is exactly what any consumer of
# UfUtilsQrCodeEncodeText has to do.
target_link_libraries(qr_code_camera_demo PRIVATE
    uflib::uflib
    ${QRENCODE_LIBRARIES}
    ${CURSES_LIBRARIES}
)

ufsrv_apply_build_options(qr_code_camera_demo)

message(STATUS "QR-code module: demo built — qr_code_camera_demo")
