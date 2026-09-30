# ── Live Redis test ───────────────────────────────────────────────────────
#
# Not a CTest gate: it needs a reachable Redis, which CI and a clean checkout
# may not have.  Built under UFLIB_CAPABILITY_HIREDIS, run deliberately:
#
#   ./ufconfig_redis_live [host] [port] [namespace]
#
# It is the only test that exercises the driver interface against a store the
# library has never seen, and the only one that gives UfConfigRegisterDriver a
# caller outside the library.
#
# The client is named here rather than by the library, because a client
# compiled in for a test must not become a dependency of every consumer.

set(_live_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
# flat: headers and sources side by side, as the generator emits them

set(_live_schema_dir ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/schema)
set(_live_schema_files
    ${_live_schema_dir}/core_schema.lua
    ${_live_schema_dir}/adjunct_schema.lua
    ${_live_schema_dir}/test_mutable.lua
)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_live_gen_root}/ufconfig_lookup_generated.c
        ${_live_gen_root}/ufconfig_fielddesc_generated.c
        ${_live_gen_root}/ufconfig_setters_stub_generated.c
        ${_live_gen_root}/ufconfig_fields_generated.h
        ${_live_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_live_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_live_schema_files}
                -o ${_live_gen_root}
                
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_live_schema_files}
    COMMENT "Generating the live test's config artefacts"
    VERBATIM
)

add_executable(ufconfig_redis_live
    redis_live_test.c
    ufconfig_driver_hiredis.c
    ${_live_gen_root}/ufconfig_lookup_generated.c
    ${_live_gen_root}/ufconfig_fielddesc_generated.c
    ${_live_gen_root}/ufconfig_setters_stub_generated.c
)

# hiredis is the consumer's to link, not the library's, and the library it links
# must have been built with the Redis capability (UFLIB_CAPABILITY_HIREDIS=ON).  This target is the
# demonstration of that arrangement: it names the client itself.
add_library(ufconfig_live_hiredis STATIC ${UFLIB_HIREDIS_SOURCES})
target_include_directories(ufconfig_live_hiredis PUBLIC
    ${CMAKE_SOURCE_DIR}/packages/hiredis
)

target_include_directories(ufconfig_redis_live PRIVATE
    ${_live_gen_root}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/packages/hiredis
)

target_link_libraries(ufconfig_redis_live PRIVATE
    uflib::uflib
    ufconfig_live_hiredis
    pthread m
)

# Where the fixture documents live, for the default run.
target_compile_definitions(ufconfig_redis_live PRIVATE
    UFCONFIG_TEST_DIR="${CMAKE_SOURCE_DIR}/tests/ufconfig"
)

ufsrv_apply_build_options(ufconfig_redis_live)
