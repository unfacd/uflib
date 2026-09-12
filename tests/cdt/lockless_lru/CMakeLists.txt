# ── Unit tests ──────────────────────────────────────────────────────────

add_c_test(
    NAME cdt_lockless_lru_tests
    TEST_SOURCE cdt_lockless_lru_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/cdt/lockless_lru
    LIBRARIES uflib::uflib pthread
)

# ── Standalone stress test executable ─────────────────────────────────────
# Not registered with CTest (requires --duration arg); build only.

add_executable(cdt_lockless_lru_stress
    cdt_lockless_lru_stress.c
)
target_include_directories(cdt_lockless_lru_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(cdt_lockless_lru_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(cdt_lockless_lru_stress)
