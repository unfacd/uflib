# ── Collection module tests ───────────────────────────────────────────────
#
# A C program with its own main() and asserts, not a gtest suite: arena and
# collector are plain C17 and predate the harness.  Registered with CTest, and
# linked against the library so the test exercises the shipped sources.

add_executable(collection_tests test_collection.c)

target_include_directories(collection_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(collection_tests PRIVATE uflib::uflib)

ufsrv_apply_build_options(collection_tests)

add_test(NAME collection_tests COMMAND collection_tests)

# Fluent stream tests. The stream headers and implementation use clang blocks;
# off Apple platforms the blocks runtime is a separate library to link.
if(APPLE)
    set(_collection_blocks_runtime)
else()
    set(_collection_blocks_runtime BlocksRuntime)
endif()

add_executable(collection_stream_tests stream/test_fluent.c)

target_include_directories(collection_stream_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(collection_stream_tests PRIVATE
    uflib::uflib
    ${_collection_blocks_runtime}
    pthread
)

ufsrv_apply_build_options(collection_stream_tests)

add_test(NAME collection_stream_tests COMMAND collection_stream_tests)

# The proposed fluent shape, running the actual fence_redis_fluent.c against
# stub hiredis/ufnet headers and an in-memory reply.
add_executable(collection_fence_fluent_sample_tests
    stream/test_fence_sample.c
    stream/fence_redis_fluent.c
)

target_include_directories(collection_fence_fluent_sample_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/stream/stubs
)

target_link_libraries(collection_fence_fluent_sample_tests PRIVATE
    uflib::uflib
    ${_collection_blocks_runtime}
    pthread
)

ufsrv_apply_build_options(collection_fence_fluent_sample_tests)

add_test(NAME collection_fence_fluent_sample_tests COMMAND collection_fence_fluent_sample_tests)

# Benchmark-only target: not registered with CTest. Build it explicitly:
#   cmake --build build/debug --target collection_stream_bench
#   taskset -c 2 build/debug/tests/collection/collection_stream_bench
add_executable(collection_stream_bench EXCLUDE_FROM_ALL
    stream/bench_stream.c
    ${CMAKE_SOURCE_DIR}/src/collection/arena.c
    ${CMAKE_SOURCE_DIR}/src/collection/collection.c
    ${CMAKE_SOURCE_DIR}/src/collection/stream/stream.c
    ${CMAKE_SOURCE_DIR}/src/collection/stream/stream_fluent.c
)

target_include_directories(collection_stream_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(collection_stream_bench PRIVATE
    ${_collection_blocks_runtime}
    pthread
)

# Benchmark sources are compiled here rather than taken from the configured
# library, and the shared option target is deliberately not applied: a Debug
# build compiles the library at -O0, while coverage/sanitizer instrumentation
# would measure the build flags instead of the implementation.
target_compile_options(collection_stream_bench PRIVATE
    -O2
    -fblocks
    -Wall
    -Wextra
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(collection_stream_bench PRIVATE BENCH_WRAP_ALLOC)
    target_link_options(collection_stream_bench PRIVATE
        -Wl,--wrap=malloc
        -Wl,--wrap=calloc
        -Wl,--wrap=realloc
    )
endif()
