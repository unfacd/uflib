# ── Unit tests (registered with CTest) ──────────────────────────────────────

add_c_test(
    NAME cdt_anderson_spinlock_tests
    TEST_SOURCE cdt_anderson_spinlock_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

# ── Standalone stress test executable (build only, not registered with CTest) ──

add_executable(cdt_anderson_spinlock_stress
    cdt_anderson_spinlock_stress.c
)
target_include_directories(cdt_anderson_spinlock_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(cdt_anderson_spinlock_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(cdt_anderson_spinlock_stress)
