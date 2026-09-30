# ── Logger-injection contract across the adt modules ──────────────────────
#
# The three adt modules that follow the agreed shape — opaque handle, created by
# a function that returns it — take an additive, borrowed UfLogger pointer at
# create time.  These cases hold that contract in place: that a NULL logger is
# exactly the old behaviour, that an injected one actually receives records,
# that a module never frees what it only borrowed, and that no record is ever
# emitted from an operational path.
#
# mock_driver.c is the C face of the logger's private driver seam and is the only
# file in this target that includes private logger headers, so the C++ test sees
# public types only.  src/logger is on the include path for that file's sake.
#
# utf8proc is not named in uflib's own exported interface, so a target that
# reaches hashtable_v2's symbols has to name it for itself.  Same resolution as
# tests/adt/hashtable_v2.

find_library(UTF8PROC_LIBRARY utf8proc PATHS /opt/lib NO_DEFAULT_PATH)

add_c_test(
    NAME adt_logger_injection_tests
    TEST_SOURCE adt_logger_injection_tests.cpp
    C_SOURCES ${CMAKE_SOURCE_DIR}/tests/logger/mock_driver.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests/logger
        ${CMAKE_SOURCE_DIR}/src/logger
        # config_uflib.h is generated here; the OBJECT target carries it as a
        # BUILD_INTERFACE, but that does not reach a target that consumes it
        # through $<TARGET_OBJECTS> rather than by linking it.
        ${CMAKE_BINARY_DIR}
    LIBRARIES uflib::uflib pthread ${UTF8PROC_LIBRARY}
)
