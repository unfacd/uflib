# ── Logger tests ──────────────────────────────────────────────────────────
#
# Two gtest binaries with deliberately different reach:
#
#   logger_core_tests   the core, against a recording double injected through
#                       the module's private hook.  zlog_s is on the link line
#                       only because libuflib.a now contains the backend and the
#                       linker pulls it in; this binary never calls into zlog,
#                       never installs a configuration, and therefore never
#                       incurs the upstream leak — so it runs under full
#                       ASan + UBSan + LSan with nothing suppressed.
#
#   logger_zlog_tests   the real backend, through the production create path.
#                       Links zlog, and therefore inherits an upstream leak
#                       that is documented in the design document's issues
#                       register (ISS-001); a narrow suppression file is
#                       attached to this test alone.
#
# src/logger is on the include path because that is where the private headers
# live.  Only mock_driver.c includes them — the C++ translation units see public
# types only, which is what keeps raw C17 atomics out of a C++ compile.

# ── Core: recording double, full sanitizers ───────────────────────────────

add_c_test(
    NAME logger_core_tests
    TEST_SOURCE logger_core_tests.cpp
    C_SOURCES mock_driver.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/logger
        # config_uflib.h is generated here; the OBJECT target carries it as a
        # BUILD_INTERFACE, but that does not reach a target that consumes it
        # through $<TARGET_OBJECTS> rather than by linking it.
        ${CMAKE_BINARY_DIR}
    LIBRARIES uflib::uflib pthread
)

# ── Real backend: production path ─────────────────────────────────────────
#
# Under the same switch as the backend it exercises.  With zlog left out there
# is no backend for the production create path to bind, so the suite has nothing
# to test — and would not link, since it calls zlog's API directly.
if(UFLIB_CAPABILITY_ZLOG)
    add_c_test(
        NAME logger_zlog_tests
        TEST_SOURCE logger_zlog_tests.cpp
        INCLUDE_DIRS
            ${CMAKE_SOURCE_DIR}/include/uflib
            ${CMAKE_SOURCE_DIR}/include
        LIBRARIES uflib::uflib pthread
    )

    # The suppression covers zlog's own configuration allocations and nothing else,
    # and it is attached to this test rather than set globally so that a leak
    # anywhere else is still reported as a failure.
    set_tests_properties(logger_zlog_tests PROPERTIES
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt:print_suppressions=1"
    )
endif()

# ── Standalone stress ─────────────────────────────────────────────────────
#
# Not registered with CTest: it takes arguments and runs for a caller-chosen
# duration, so it is a tool rather than a gate.  It links zlog for the
# --driver zlog mode, but only ever holds a single logger, so the upstream leak
# is incurred once rather than per iteration.

add_executable(logger_stress
    logger_stress.c
    mock_driver.c
)

target_include_directories(logger_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/logger
    ${CMAKE_BINARY_DIR}
)

target_link_libraries(logger_stress PRIVATE
    uflib::uflib pthread m
)

ufsrv_apply_build_options(logger_stress)
