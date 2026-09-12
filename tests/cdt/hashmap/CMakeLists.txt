# ── Unit tests ──────────────────────────────────────────────────────────

add_c_test(
    NAME cdt_fixed_width_hashmap_tests
    TEST_SOURCE cdt_fixed_width_hashmap_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

# ── Standalone stress test executable ─────────────────────────────────────
# Not registered with CTest (requires --duration arg); build only.

add_executable(cdt_fixed_width_hashmap_stress
    cdt_fixed_width_hashmap_stress.c
)
target_include_directories(cdt_fixed_width_hashmap_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(cdt_fixed_width_hashmap_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(cdt_fixed_width_hashmap_stress)
