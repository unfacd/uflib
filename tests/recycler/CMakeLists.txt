# ── Unit tests (registered with CTest) ──────────────────────────────────────

add_c_test(
    NAME recycler_tests
    TEST_SOURCE recycler_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/recycler
    LIBRARIES uflib::uflib pthread
)

# ── Standalone stress test executable (build only, not registered with CTest) ──

add_executable(recycler_stress
    recycler_stress.c
)
target_include_directories(recycler_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/recycler
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(recycler_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(recycler_stress)
