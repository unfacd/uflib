add_c_test(
    NAME scheduled_jobs_tests
    TEST_SOURCE scheduled_jobs_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

add_c_test(
    NAME scheduled_jobs_adversarial_tests
    TEST_SOURCE scheduled_jobs_adversarial_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

# Standalone stress test (not in CTest)
add_executable(scheduled_jobs_stress
    scheduled_jobs_stress.c
)
target_include_directories(scheduled_jobs_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(scheduled_jobs_stress PRIVATE uflib::uflib pthread m)
target_compile_options(scheduled_jobs_stress PRIVATE -fblocks)
ufsrv_apply_build_options(scheduled_jobs_stress)

# Rigorous MT stress test (not in CTest) — verifies order + no-lost-jobs
add_executable(scheduled_jobs_mt_stress
    scheduled_jobs_mt_stress.c
)
target_include_directories(scheduled_jobs_mt_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(scheduled_jobs_mt_stress PRIVATE uflib::uflib pthread m)
target_compile_options(scheduled_jobs_mt_stress PRIVATE -fblocks)
ufsrv_apply_build_options(scheduled_jobs_mt_stress)
