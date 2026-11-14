# ── Unit tests ──────────────────────────────────────────────────────────

add_c_test(
    NAME cdt_alias_map_tests
    TEST_SOURCE alias_map_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

# ── Standalone stress test executable ─────────────────────────────────────
# Not registered with CTest (accepts --threads/--duration/--capacity).

add_executable(cdt_alias_map_stress
    alias_map_stress.c
)
target_include_directories(cdt_alias_map_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(cdt_alias_map_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(cdt_alias_map_stress)

# ── Allocation-failure injection ──────────────────────────────────────────
# Compiles the module source directly so -Wl,--wrap reliably intercepts its
# malloc/calloc calls, which a shared-library link would not.

add_executable(cdt_alias_map_oom
    alias_map_oom.c
    ${CMAKE_SOURCE_DIR}/src/cdt/alias_map/alias_map.c
    ${CMAKE_SOURCE_DIR}/src/cdt/alias_map/alias_map_json.c
)
target_include_directories(cdt_alias_map_oom PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/cdt/alias_map
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(cdt_alias_map_oom PRIVATE uflib::uflib pthread)
target_link_options(cdt_alias_map_oom PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc)
ufsrv_apply_build_options(cdt_alias_map_oom)
