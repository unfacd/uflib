#
# tests/CMakeLists.txt — gtest harness for uflib
#
# Provides:
#   gtest_shared_main      OBJECT library linked into every test executable
#   add_c_test()           one-call test registration (executable + CTest)
#

# ── Test link dependencies ────────────────────────────────────────────────

find_library(UTF8PROC_LIBRARY utf8proc PATHS /opt/lib NO_DEFAULT_PATH)

# ── Shared gtest main ─────────────────────────────────────────────────────

add_library(gtest_shared_main OBJECT main.cpp)
target_link_libraries(gtest_shared_main PUBLIC GTest::gtest GTest::gtest_main)
target_compile_options(gtest_shared_main PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fblocks>)
ufsrv_apply_build_options(gtest_shared_main)

# ── add_c_test() macro ────────────────────────────────────────────────────

function(add_c_test)
    set(options "")
    set(oneValueArgs NAME TEST_SOURCE)
    set(multiValueArgs C_SOURCES INCLUDE_DIRS LIBRARIES)
    cmake_parse_arguments(TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${TEST_NAME}
        ${TEST_TEST_SOURCE}
        $<TARGET_OBJECTS:gtest_shared_main>
        ${TEST_C_SOURCES}
    )
    set_target_properties(${TEST_NAME} PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(${TEST_NAME} PRIVATE
        GTest::gtest GTest::gtest_main stdc++ m
        ${TEST_LIBRARIES}
    )
    ufsrv_apply_build_options(${TEST_NAME})
    # C++ test files include C headers that use Apple Blocks — need -fblocks for CXX too
    target_compile_options(${TEST_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fblocks>)
    add_test(NAME ${TEST_NAME} COMMAND $<TARGET_FILE:${TEST_NAME}>)
endfunction()

# ── Test subdirectories ───────────────────────────────────────────────────

add_subdirectory(utils)
add_subdirectory(exponential_backoff)
add_subdirectory(ufsrvuid)
add_subdirectory(recycler)
add_subdirectory(recycler_v2)
add_subdirectory(buffer_descriptor)

# ADT tests
add_subdirectory(adt/distinct_array)
add_subdirectory(adt/hashtable_v2)
add_subdirectory(adt/hopscotch)
add_subdirectory(adt/hopscotch_hashtable_v2)
add_subdirectory(adt/minheap)

# CDT tests
add_subdirectory(cdt/anderson_spinlock)
add_subdirectory(cdt/chase_lev_stealing_queue)
add_subdirectory(cdt/hashmap)
add_subdirectory(cdt/lamport_queue)
add_subdirectory(cdt/lockless_lru)
add_subdirectory(cdt/lockless_minheap)
add_subdirectory(cdt/lockless_ringbuffer)
add_subdirectory(cdt/mcs_lock)
add_subdirectory(cdt/mpsc_queue)

# Scheduled jobs tests
add_subdirectory(scheduled_jobs)

# Thread worker tests
add_subdirectory(thread_worker/file_loader_concurrent_access)
