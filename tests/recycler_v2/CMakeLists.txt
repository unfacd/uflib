# RecyclerV2 test targets — gated behind _PACKAGE_TESTS (guarded by parent)

add_c_test(
    NAME recycler_v2_tests
    TEST_SOURCE recycler_v2_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/recycler_v2
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
