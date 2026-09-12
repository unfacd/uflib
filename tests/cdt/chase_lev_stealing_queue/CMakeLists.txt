# ── Unit tests ──────────────────────────────────────────────────────────

add_c_test(
    NAME cdt_chase_lev_stealing_queue_tests
    TEST_SOURCE cdt_chase_lev_stealing_queue_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/cdt/chase_lev_stealing_queue
    LIBRARIES uflib::uflib pthread
)

# ── Standalone stress test executables ─────────────────────────────────────
# Not registered with CTest (own main); build only.

add_executable(cdt_chase_lev_stealing_queue_stress
    cdt_chase_lev_stealing_queue_stress.c
)
target_include_directories(cdt_chase_lev_stealing_queue_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(cdt_chase_lev_stealing_queue_stress PRIVATE
    uflib::uflib pthread
)
ufsrv_apply_build_options(cdt_chase_lev_stealing_queue_stress)

add_executable(cdt_chase_lev_stealing_queue_stress_mixed
    cdt_chase_lev_stealing_queue_stress_mixed.c
)
target_include_directories(cdt_chase_lev_stealing_queue_stress_mixed PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(cdt_chase_lev_stealing_queue_stress_mixed PRIVATE
    uflib::uflib pthread
)
ufsrv_apply_build_options(cdt_chase_lev_stealing_queue_stress_mixed)
