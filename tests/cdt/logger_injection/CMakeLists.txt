# ── Logger-injection contract across the cdt modules ──────────────────────
#
# Five lock-free structures in src/cdt/ take an additive, borrowed UfLogger
# pointer at create time.  These cases hold that contract in place: that a NULL
# logger is exactly the old behaviour, that an injected one actually receives
# records, that a module never frees what it only borrowed, and that no record
# is ever emitted from a hot path.
#
# mock_driver.c is the C face of the logger's private driver seam and is the only
# file in this target that includes private logger headers, so the C++ test sees
# public types only.  src/logger is on the include path for that file's sake.

add_c_test(
    NAME cdt_logger_injection_tests
    TEST_SOURCE cdt_logger_injection_tests.cpp
    C_SOURCES ${CMAKE_SOURCE_DIR}/tests/logger/mock_driver.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests/logger
        ${CMAKE_SOURCE_DIR}/src/logger
        ${CMAKE_SOURCE_DIR}/src/cdt/lockless_ringbuffer
        ${CMAKE_SOURCE_DIR}/src/cdt/lockless_lru
        ${CMAKE_SOURCE_DIR}/src/cdt/lockless_minheap
        ${CMAKE_SOURCE_DIR}/src/cdt/lockless_treiber_stack
        ${CMAKE_SOURCE_DIR}/src/cdt/chase_lev_stealing_queue
        # config_uflib.h is generated here; the OBJECT target carries it as a
        # BUILD_INTERFACE, but that does not reach a target that consumes it
        # through $<TARGET_OBJECTS> rather than by linking it.
        ${CMAKE_BINARY_DIR}
    LIBRARIES uflib::uflib pthread
)
