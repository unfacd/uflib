add_c_test(
    NAME buffer_descriptor_tests
    TEST_SOURCE buffer_descriptor_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)
