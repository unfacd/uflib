# ── Config module tests ───────────────────────────────────────────────────
#
# Six gtest binaries, split by what they reach rather than by size:
#
#   ufconfig_core_tests        load, accessors, limits.  Public API only.
#   ufconfig_corpus_tests      the parse corpus, driven by verdicts.tsv.
#   ufconfig_gate_tests        state left behind by failed operations.
#   ufconfig_namespace_tests   tree walking, and the ownership rule with it.
#   ufconfig_driver_tests      the driver boundary: C1 and C3 round-tripping.
#   ufconfig_transform_tests   the transform catalogue and the validators.
#
# The last one is separate because it is the only one that touches the module's
# internals.  Private headers pull in C17 <stdatomic.h>, which does not survive a
# C++ translation unit, so the include lives in a C shim
# (ufconfig_internals_shim.c) and the test sees plain declarations.  Keeping it
# in its own binary means the other five provably include no private header —
# src/ufconfig is on the include path for this target only.
#
# Fixture locations are configured rather than written into the sources: CTest
# runs each test from the build directory, so a path relative to the source file
# would be wrong at run time.  See ufconfig_test_paths.h.in.

set(_ufconfig_test_root ${CMAKE_SOURCE_DIR}/src/ufconfig)

# The suite's schema.  Each test binary is a consumer, so each links the
# generated artefacts — the library carries none.
set(_tst_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
set(_tst_schema_dir ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/schema)
set(_tst_schemas
    ${_tst_schema_dir}/core_schema.lua
    ${_tst_schema_dir}/adjunct_schema.lua
    ${_tst_schema_dir}/test_mutable.lua)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_tst_gen_root}/ufconfig_lookup_generated.c
        ${_tst_gen_root}/ufconfig_fielddesc_generated.c
        ${_tst_gen_root}/ufconfig_setters_stub_generated.c
        ${_tst_gen_root}/ufconfig_fields_generated.h
        ${_tst_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_tst_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_tst_schemas} -o ${_tst_gen_root}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_tst_schemas}
    COMMENT "Generating the test suite's schema artefacts"
    VERBATIM
)

set(_tst_gen_sources
    ${_tst_gen_root}/ufconfig_lookup_generated.c
    ${_tst_gen_root}/ufconfig_fielddesc_generated.c
    ${_tst_gen_root}/ufconfig_setters_stub_generated.c)

set(UFCONFIG_TEST_FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(UFCONFIG_TEST_SAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/examples)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/ufconfig_test_paths.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/ufconfig_test_paths.h
    @ONLY
)

set(_ufconfig_test_includes
    ${_tst_gen_root}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_BINARY_DIR}
)

# ── Public-API suites ─────────────────────────────────────────────────────

add_c_test(
    NAME ufconfig_core_tests
    TEST_SOURCE ufconfig_core_tests.cpp
    C_SOURCES ${_tst_gen_sources}
    INCLUDE_DIRS ${_ufconfig_test_includes}
    LIBRARIES uflib::uflib pthread
)

add_c_test(
    NAME ufconfig_corpus_tests
    TEST_SOURCE ufconfig_corpus_tests.cpp
    C_SOURCES ${_tst_gen_sources}
    INCLUDE_DIRS ${_ufconfig_test_includes}
    LIBRARIES uflib::uflib pthread
)

add_c_test(
    NAME ufconfig_gate_tests
    TEST_SOURCE ufconfig_gate_tests.cpp
    C_SOURCES ${_tst_gen_sources}
    INCLUDE_DIRS ${_ufconfig_test_includes}
    LIBRARIES uflib::uflib pthread
)

add_c_test(
    NAME ufconfig_namespace_tests
    TEST_SOURCE ufconfig_namespace_tests.cpp
    C_SOURCES ${_tst_gen_sources}
    INCLUDE_DIRS ${_ufconfig_test_includes}
    LIBRARIES uflib::uflib pthread
)

add_c_test(
    NAME ufconfig_driver_tests
    TEST_SOURCE ufconfig_driver_tests.cpp
    C_SOURCES ${_tst_gen_sources}
    INCLUDE_DIRS ${_ufconfig_test_includes}
    LIBRARIES uflib::uflib pthread
)

# ── Internals suite ───────────────────────────────────────────────────────
# The only target that gets src/ufconfig on its include path, and the only one
# that compiles the shim.

add_c_test(
    NAME ufconfig_transform_tests
    TEST_SOURCE ufconfig_transform_tests.cpp
    C_SOURCES ufconfig_internals_shim.c
    INCLUDE_DIRS
        ${_ufconfig_test_includes}
        ${_ufconfig_test_root}
    LIBRARIES uflib::uflib pthread
)

# ── Standalone stress ─────────────────────────────────────────────────────
#
# Not registered with CTest: it takes a thread count and a duration, so it is a
# tool rather than a gate.  Run it directly:
#
#   ./ufconfig_stress --threads 8 --duration 10 --root <repo>/src/ufconfig
#
# --root is the module directory, because the stress writes paths as
# <root>/sample/sample.config.lua.

# The stress tool is a consumer too, so it needs its own schema artefacts like
# every other harness here — it was written before the schema was injected and
# was the last thing still calling the old constructor.
set(_stress_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
set(_stress_schema_dir ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/schema)
set(_stress_schemas
    ${_stress_schema_dir}/core_schema.lua
    ${_stress_schema_dir}/adjunct_schema.lua
    ${_stress_schema_dir}/test_mutable.lua)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_stress_gen_root}/ufconfig_lookup_generated.c
        ${_stress_gen_root}/ufconfig_fielddesc_generated.c
        ${_stress_gen_root}/ufconfig_setters_stub_generated.c
        ${_stress_gen_root}/ufconfig_fields_generated.h
        ${_stress_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_stress_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_stress_schemas} -o ${_stress_gen_root}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_stress_schemas}
    COMMENT "Generating the stress tool's schema artefacts"
    VERBATIM
)

add_executable(ufconfig_stress
    ufconfig_stress.c
    ${_stress_gen_root}/ufconfig_lookup_generated.c
    ${_stress_gen_root}/ufconfig_fielddesc_generated.c
    ${_stress_gen_root}/ufconfig_setters_stub_generated.c
)

target_include_directories(ufconfig_stress PRIVATE
    ${_stress_gen_root}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ufconfig_stress PRIVATE uflib::uflib pthread m)
ufsrv_apply_build_options(ufconfig_stress)

#
# ufconfig_bench — load and lookup cost at the module's choke points.
#
# Not registered with CTest: it takes arguments and runs for a measured
# duration, so it is a tool rather than a gate.  Run it directly:
#
#   ./ufconfig_bench --sample <repo>/tests/ufconfig/examples/sample.strict.lua
#   ./ufconfig_bench --sample <f> --record    <repo>/tests/ufconfig/bench/baseline.tsv
#   ./ufconfig_bench --sample <f> --baseline  <repo>/tests/ufconfig/bench/baseline.tsv
#
# Unlike every other harness here it does NOT link uflib::uflib.  It compiles
# the module's own sources at -O2 with sanitizers disabled, because a benchmark
# linked against a sanitizer-instrumented library measures the sanitizer: ASan
# moves these timings by a large factor, and no figure from an instrumented
# build is comparable with one from an uninstrumented build.  Compiling the
# sources directly also makes the numbers independent of whichever preset the
# rest of the tree happens to be configured with — otherwise the same benchmark
# reports different numbers in `dev` and in `release` for no reason but flags.

set(_bench_module_dir ${CMAKE_SOURCE_DIR}/src/ufconfig)
set(_bench_module_sources
    ${_bench_module_dir}/ufconfig.c
    ${_bench_module_dir}/ufconfig_canon.c
    ${_bench_module_dir}/ufconfig_namespace.c
    ${_bench_module_dir}/ufconfig_pairs.c
    ${_bench_module_dir}/ufconfig_parse.c
    ${_bench_module_dir}/ufconfig_transform.c
    ${_bench_module_dir}/ufconfig_util.c
    ${_bench_module_dir}/ufconfig_validate.c
    ${_bench_module_dir}/driver/ufconfig_driver.c
    ${_bench_module_dir}/driver/ufconfig_driver_file.c
    ${_bench_module_dir}/driver/ufconfig_driver_mem.c)

# The opt-in drivers are absent by default, and that is not a shortcut: this
# target compiles the module's sources by hand rather than linking uflib, so it
# inherits neither the library's include paths nor its link line, and listing
# ufconfig_driver_sql.c here unconditionally is what broke every build — it
# failed on a mariadb/mysql.h this target has no path to.
#
# But its copy of ufconfig_driver.c reads the capability macros from
# config_uflib.h like any other translation unit, so with a capability ON that
# object registers a driver this target has not compiled, and the link fails on
# a symbol rather than a header.  Compiling for the configuration in force means
# taking the driver, its include path and its client when the capability is set,
# and none of the three when it is not.
if(UFLIB_CAPABILITY_SQL)
    list(APPEND _bench_module_sources ${_bench_module_dir}/driver/ufconfig_driver_sql.c)
endif()
if(UFLIB_CAPABILITY_HIREDIS)
    list(APPEND _bench_module_sources ${_bench_module_dir}/driver/ufconfig_driver_redis.c)
endif()

add_executable(ufconfig_bench
    bench/ufconfig_bench.c
    bench/ufconfig_bench_stubs.c
    bench/ufconfig_bench_secrets_stubs.c
    ${_bench_module_sources}
    ${_tst_gen_sources})

target_include_directories(ufconfig_bench PRIVATE
    ${_bench_module_dir}
    ${_tst_gen_root}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR})

target_compile_options(ufconfig_bench PRIVATE
    -std=gnu17 -D_GNU_SOURCE -O2 -Wall -Wextra -fno-sanitize=all -fno-omit-frame-pointer)
target_link_options(ufconfig_bench PRIVATE -fno-sanitize=all)
target_link_libraries(ufconfig_bench PRIVATE pthread m)

# The include path and the client for whichever opt-in driver the source list
# above just took — see the note there.  After add_executable, because a target
# has to exist before it can be given anything.
if(UFLIB_CAPABILITY_SQL)
    target_include_directories(ufconfig_bench PRIVATE /opt/include)
    target_link_libraries(ufconfig_bench PRIVATE ${MariaDB_LIBRARIES})
endif()
if(UFLIB_CAPABILITY_HIREDIS)
    target_include_directories(ufconfig_bench PRIVATE ${CMAKE_SOURCE_DIR}/packages/hiredis)
    target_link_libraries(ufconfig_bench PRIVATE ufconfig_test_hiredis)
endif()
