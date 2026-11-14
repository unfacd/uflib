# ── Unit and concurrency tests ──────────────────────────────────────────

add_c_test(
    NAME resource_router_tests
    TEST_SOURCE resource_router_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

# ── Standalone stress / benchmark executable ──────────────────────────────
# Not registered with CTest (requires arguments); build only.

add_executable(resource_router_stress
    resource_router_stress.c
)
target_include_directories(resource_router_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(resource_router_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(resource_router_stress)
