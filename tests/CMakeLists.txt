#
# tests/CMakeLists.txt — gtest harness for uflib
#
# Provides:
#   gtest_shared_main      OBJECT library linked into every test executable
#   add_c_test()           one-call test registration (executable + CTest)
#

# ── Test link dependencies ────────────────────────────────────────────────

# Named only with the capability: without it nothing in the archive references
# utf8proc, and targets here list ${UTF8PROC_LIBRARY} unconditionally — an unset
# variable expands to nothing.
if(UFLIB_CAPABILITY_UTF8PROC)
    find_package(Utf8proc REQUIRED)
    set(UTF8PROC_LIBRARY ${UTF8PROC_LIBRARIES})
endif()

# ── The SQL client, for the test tree only ────────────────────────────────
#
# uflib does not name libmariadb in its interface, on purpose: a host that talks
# to SQL names the client it talks with, exactly as it does for hiredis.  But
# every target in this tree that uses the config module pulls ufconfig_driver.o
# out of the archive, and with the SQL capability on that object references the
# client — so the test tree has to name it, once, here, rather than in each of
# the thirty-odd test files that would otherwise need their own line.
#
# The library comes from the `find_package(MariaDB)` the top level ran beside
# the capability, so there is no second lookup here and no second cache entry to
# disagree with it.
#
# link_libraries() is directory-scoped and inherited by the subdirectories added
# below, which is the scope wanted: the test tree, not the library.
if(UFLIB_CAPABILITY_SQL)
    link_libraries(${MariaDB_LIBRARIES})
endif()

# ── The Redis client, for the test tree only ──────────────────────────────
#
# The library leaves the client to the consumer to name, so the tests name it
# here, from the vendored package's own source list rather than a copy of it.
#
# SHARED, and that is load-bearing rather than incidental.  link_libraries()
# prepends, so a static archive here lands *before* libuflib.a -- and the
# linker resolves archives left to right, so it discards hiredis as unneeded,
# then pulls the Redis driver out of libuflib and finds its symbols with
# nothing left to bind them to.  A shared object carries no such ordering
# requirement, which is also why the libmariadb line above works: that is a .so.
if(UFLIB_CAPABILITY_HIREDIS)
    add_library(ufconfig_test_hiredis SHARED ${UFLIB_HIREDIS_SOURCES})
    set_target_properties(ufconfig_test_hiredis PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ufconfig_test_hiredis PUBLIC
        ${CMAKE_SOURCE_DIR}/packages/hiredis
    )
    link_libraries(ufconfig_test_hiredis)
endif()

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
add_subdirectory(optional)

# The vendored parser's extension, gated on the parser
add_subdirectory(mjson)

# ADT tests
add_subdirectory(adt/distinct_array)
add_subdirectory(adt/hashtable_v2)
add_subdirectory(adt/hopscotch)
add_subdirectory(adt/hopscotch_hashtable_v2)
add_subdirectory(adt/minheap)
add_subdirectory(adt/logger_injection)

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
add_subdirectory(cdt/logger_injection)

# Scheduled jobs tests
add_subdirectory(scheduled_jobs)

# Thread worker tests
add_subdirectory(thread_worker/file_loader_concurrent_access)

# Logger tests
add_subdirectory(logger)

# Config module tests
add_subdirectory(ufconfig)

# Command module tests
add_subdirectory(ufcommand)

# Command module — sample host application (a consumer, not a test)
add_subdirectory(ufcommand/examples)

# Config module — sample host application (a consumer, not a test)
add_subdirectory(ufconfig/examples)

# Config module — live Redis test (needs a running store; not a CTest gate).
# Under the same capability as everything else Redis: the client is vendored,
# but there is nothing for this to talk to when the library carries no driver.
if(UFLIB_CAPABILITY_HIREDIS)
    add_subdirectory(ufconfig/redis_live)
endif()

# Config module — live SQL test (needs a running MariaDB; not a CTest gate)
add_subdirectory(ufconfig/sql_live)

# Config module — SQL round trip: load a document in C, diff the store in Python
add_subdirectory(ufconfig/sql_roundtrip)

# Config module — transform and validator coverage (prints; read, not asserted)
add_subdirectory(ufconfig/coverage)

# Config module — schema rejection (prints; the lines are the evidence)
add_subdirectory(ufconfig/negative)

# Config module — standalone validator over real config files
add_subdirectory(ufconfig/validate)

# Config module — serialiser golden-file tests
add_subdirectory(ufconfig/serialise)

# Config module — encrypted values (`encrypted = true`), against its own schema
add_subdirectory(ufconfig/secrets)

# QR-code module — gated here as well as inside, so that a build without either
# capability does not enter the directory at all rather than entering it to add
# nothing.
if(UFLIB_CAPABILITY_QUIRC OR UFLIB_CAPABILITY_QRENCODE)
    add_subdirectory(qr_code)
endif()
