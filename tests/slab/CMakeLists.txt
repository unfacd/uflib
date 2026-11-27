add_c_test(
    NAME inlined_slab_tests
    TEST_SOURCE inlined_slab_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)
