# ── Unit tests (registered with CTest) ──────────────────────────────────────

find_library(UTF8PROC_LIBRARY utf8proc PATHS /opt/lib NO_DEFAULT_PATH)

add_c_test(
    NAME hashtable_v2_tests
    TEST_SOURCE hashtable_v2_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/adt/hashtable_v2
    LIBRARIES uflib::uflib pthread ${UTF8PROC_LIBRARY}
)

# ── Standalone stress test executable (build only, not registered with CTest) ──

add_executable(hashtable_v2_stress
    hashtable_v2_stress.c
)
target_include_directories(hashtable_v2_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/adt/hashtable_v2
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(hashtable_v2_stress PRIVATE
    uflib::uflib pthread m ${UTF8PROC_LIBRARY}
)
ufsrv_apply_build_options(hashtable_v2_stress)
