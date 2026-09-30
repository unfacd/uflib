# Optional module tests — gated behind _PACKAGE_TESTS (guarded by parent)

add_c_test(
    NAME ufoptional_tests
    TEST_SOURCE ufoptional_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)
