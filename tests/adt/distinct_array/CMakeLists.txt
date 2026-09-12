add_c_test(
    NAME distinct_array_tests
    TEST_SOURCE distinct_array_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib BlocksRuntime ${UTF8PROC_LIBRARY}
)
