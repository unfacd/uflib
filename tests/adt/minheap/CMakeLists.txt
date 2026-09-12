# ── Unit tests ──────────────────────────────────────────────────────────

add_c_test(
    NAME adt_minheap_tests
    TEST_SOURCE adt_minheap_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)

# ── Standalone stress test executable ─────────────────────────────────────
# Not registered with CTest (requires --threads/--ops/--mutex args); build only.

add_executable(adt_minheap_stress
    adt_minheap_stress.c
)
target_include_directories(adt_minheap_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(adt_minheap_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(adt_minheap_stress)
