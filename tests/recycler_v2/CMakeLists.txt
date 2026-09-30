# RecyclerV2 test targets — gated behind _PACKAGE_TESTS (guarded by parent)

add_c_test(
    NAME recycler_v2_tests
    TEST_SOURCE recycler_v2_tests.cpp
    # The logger's recording double, so the Describe tests can assert the
    # per-pool logger attribute without pulling zlog into this binary.
    C_SOURCES ${CMAKE_SOURCE_DIR}/tests/logger/mock_driver.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/recycler_v2
        ${CMAKE_SOURCE_DIR}/tests/logger
        ${CMAKE_SOURCE_DIR}/src/logger
        # config_uflib.h is generated here; needed by mock_driver.c.
        ${CMAKE_BINARY_DIR}
    LIBRARIES uflib::uflib pthread
)

# Standalone stress test — built but NOT registered with CTest (requires CLI args)
add_executable(recycler_v2_stress recycler_v2_stress.c)
target_include_directories(recycler_v2_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/recycler_v2
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(recycler_v2_stress PRIVATE uflib::uflib pthread m)
ufsrv_apply_build_options(recycler_v2_stress)
