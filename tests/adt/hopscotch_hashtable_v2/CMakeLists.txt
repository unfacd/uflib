add_c_test(
    NAME hopscotch_hashtable_v2_tests
    TEST_SOURCE hopscotch_hashtable_v2_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/adt/hopscotch_hashtable_v2
    LIBRARIES uflib::uflib pthread
)

# ── Gate 2.8 / 7.6: Opaque boundary compile check (D-8 / HOP-033) ────
#
# CTest test: compiles try_compile_opaque_boundary.c with -fsyntax-only.
# If compilation succeeds, the opaque boundary is broken and the test fails.
add_test(
    NAME hopscotch_opaque_boundary
    COMMAND ${CMAKE_COMMAND}
        -DCOMPILER=${CMAKE_C_COMPILER}
        -DINCLUDE_DIR=${CMAKE_SOURCE_DIR}/include
        -DSOURCE_FILE=${CMAKE_SOURCE_DIR}/src/adt/hopscotch_hashtable_v2/try_compile_opaque_boundary.c
        -P ${CMAKE_SOURCE_DIR}/cmake/check_opaque_boundary.cmake
)

# Standalone stress test (not in CTest)
add_executable(hopscotch_hashtable_v2_stress
    hopscotch_hashtable_v2_stress.c
)
target_include_directories(hopscotch_hashtable_v2_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/adt/hopscotch_hashtable_v2
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(hopscotch_hashtable_v2_stress PRIVATE uflib::uflib pthread m)
ufsrv_apply_build_options(hopscotch_hashtable_v2_stress)
