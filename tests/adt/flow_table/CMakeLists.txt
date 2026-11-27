add_c_test(
    NAME flow_table_tests
    TEST_SOURCE flow_table_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

add_executable(flow_table_stress flow_table_stress.c)
target_include_directories(flow_table_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(flow_table_stress PRIVATE uflib::uflib pthread)
ufsrv_apply_build_options(flow_table_stress)
