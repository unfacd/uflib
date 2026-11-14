# ── gtest unit tests ────────────────────────────────────────────────────────

add_c_test(
    NAME file_loader_concurrent_access_tests
    TEST_SOURCE file_loader_concurrent_access_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

# ── Standalone multi-threaded stress test (not registered with CTest) ────────

add_executable(file_loader_concurrent_access_stress
    file_loader_concurrent_access_stress.c
)
target_include_directories(file_loader_concurrent_access_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_compile_definitions(file_loader_concurrent_access_stress PRIVATE
    FILE_LOADER_CONCURRENT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_link_libraries(file_loader_concurrent_access_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(file_loader_concurrent_access_stress)
