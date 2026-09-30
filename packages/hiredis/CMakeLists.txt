# packages/hiredis/CMakeLists.txt — vendored hiredis (BSD-3-Clause)
#
# Redis C client, copied verbatim from ufsrvcli/packages/hiredis/.  Linked by
# the config module's Redis driver and by nothing else in uflib — hiredis is
# confined to one translation unit, so replacing the transport is a change to
# that file rather than to the library.
#
# async.c is omitted: the driver is synchronous.  sslio.c is omitted: no TLS
# path is wired.  Adding either back is one line.

# Absolute, and published to the parent scope: the test tree wraps this same
# client in a SHARED target, and a second hand-written list would drift.
set(UFLIB_HIREDIS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dict.c
    ${CMAKE_CURRENT_SOURCE_DIR}/hiredis.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net.c
    ${CMAKE_CURRENT_SOURCE_DIR}/read.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sds.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sockcompat.c
)
set(UFLIB_HIREDIS_SOURCES "${UFLIB_HIREDIS_SOURCES}" PARENT_SCOPE)

add_library(uflib_hiredis STATIC ${UFLIB_HIREDIS_SOURCES})

target_include_directories(uflib_hiredis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

set_target_properties(uflib_hiredis PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Note: ufsrv_apply_build_options intentionally NOT applied — third-party code.
