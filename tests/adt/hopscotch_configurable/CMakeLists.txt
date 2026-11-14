#
# V1 configurable hopscotch store — identity set with a caller-supplied hash.
#
# The suite covers the whole public surface including growth, which is the only
# allocation the table performs after initialisation; the stress and perf
# binaries are standalone because they take arguments and are run by hand.
#
add_c_test(
    NAME hopscotch_configurable_tests
    TEST_SOURCE hopscotch_configurable_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

# Standalone stress test (not in CTest), model-checked against every operation.
add_executable(hopscotch_configurable_stress
    hopscotch_configurable_stress.c
)
target_include_directories(hopscotch_configurable_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(hopscotch_configurable_stress PRIVATE uflib::uflib pthread)
ufsrv_apply_build_options(hopscotch_configurable_stress)

# Standalone performance harness (not in CTest).  Record its output in
# section 9.4 of dev/technical_designs/UFLIB_ADT_HOPSCOTCH_HASHTABLE_DESIGN.md when the
# table changes; run it from an optimised, unsanitised build or the numbers mean nothing.
add_executable(hopscotch_configurable_perf
    hopscotch_configurable_perf.c
)
target_include_directories(hopscotch_configurable_perf PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(hopscotch_configurable_perf PRIVATE uflib::uflib)
ufsrv_apply_build_options(hopscotch_configurable_perf)
