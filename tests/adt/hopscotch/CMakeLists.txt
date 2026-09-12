# V1 regression tests — validate Gate 1 bug fixes before V2 module is built.
#
# These tests exercise the existing V1 public API (adt_hopscotch_hashtable.h)
# and are independent of the V2 module.  They verify:
#   Bug A: Keyed variant resize preserves all data
#   Bug B: Remove with small integer key does not segfault
#   Bug C: Init with NULL ht allocates correct struct size

add_c_test(
    NAME hopscotch_v1_regression_tests
    TEST_SOURCE hopscotch_v1_regression_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)
