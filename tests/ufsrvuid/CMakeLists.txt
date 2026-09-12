add_c_test(
    NAME ufsrvuid_tests
    TEST_SOURCE ufsrvuid_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)
