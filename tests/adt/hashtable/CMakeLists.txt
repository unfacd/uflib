add_c_test(
    NAME hashtable_tests
    TEST_SOURCE hashtable_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}
    LIBRARIES uflib::uflib pthread
)

add_executable(hashtable_cdt_stress
    hashtable_cdt_stress.c
)
target_include_directories(hashtable_cdt_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(hashtable_cdt_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(hashtable_cdt_stress)

add_c_test(
    NAME hashtable_optional_tests
    TEST_SOURCE hashtable_optional_tests.cpp
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/include/uflib ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}
    LIBRARIES uflib::uflib pthread
)
target_link_options(hashtable_optional_tests PRIVATE -Wl,--wrap=malloc)
