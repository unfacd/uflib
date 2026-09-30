# ── Live SQL test ─────────────────────────────────────────────────────────
#
# Not a CTest gate: it needs a reachable MariaDB, which CI and a clean checkout
# may not have.  Run it deliberately:
#
#   ./ufconfig_sql_live [dsn] [namespace]
#   # defaults: host=127.0.0.1;port=33061;user=root;database=ufconfig_live
#
# The SQL counterpart of ufconfig_redis_live: the module core against a store
# the library has never seen, with the driver supplied by the test rather than
# compiled into the library.  It is the only place UfConfigRegisterDriver has a
# caller from outside.
#
# The Redis twin is gated by UFLIB_CAPABILITY_HIREDIS, because the client it
# compiles is vendored and that capability is what builds it.  libmariadb is
# neither vendored nor part of a clean checkout, so this target gates on the
# client being found instead — the flag that enables the library's own SQL
# driver is not what decides it.  Nothing here links the client into the
# library; the library never sees it.

# The client comes from the find_package the top level ran beside the
# capability, so this tree does not look for it a second time.
if(NOT MariaDB_FOUND)
    message(STATUS "Config module: SQL capability off, or libmariadb not found — ufconfig_sql_live not built")
    return()
endif()

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

add_executable(ufconfig_sql_live
    sql_live_test.c
    ufconfig_driver_mariadb.c
    ${_live_gen_root}/ufconfig_lookup_generated.c
    ${_live_gen_root}/ufconfig_fielddesc_generated.c
    ${_live_gen_root}/ufconfig_setters_stub_generated.c
)

# libmariadb is the consumer's to link, not the library's, and this target is
# the demonstration of that arrangement: it names the client itself.
target_include_directories(ufconfig_sql_live PRIVATE
    ${_live_gen_root}
    ${CMAKE_SOURCE_DIR}/include
    /opt/include
    /opt/include/mariadb
)

target_link_libraries(ufconfig_sql_live PRIVATE
    uflib::uflib
    ${MariaDB_LIBRARIES}
    pthread m
)

# Where the fixture documents live, and where the schema the test applies is
# kept.  The DDL is read from the source tree rather than copied, so the file
# applied by the test is the same file a deployment applies.
target_compile_definitions(ufconfig_sql_live PRIVATE
    UFCONFIG_TEST_DIR="${CMAKE_SOURCE_DIR}/tests/ufconfig"
    UFCONFIG_DDL_PATH="${CMAKE_SOURCE_DIR}/src/ufconfig/driver/ufconfig_mariadb.sql"
)

ufsrv_apply_build_options(ufconfig_sql_live)

# ── The same run against the library's own driver ─────────────────────────
#
# Nothing else in the tree exercises src/ufconfig/driver/ufconfig_driver_sql.c
# against a real server.  The target above supplies its own driver by design —
# that is what makes it evidence about the interface — and the gtest suite's SQL
# case skips without a DSN.  This second target compiles the same harness with
# UFCONFIG_LIVE_USE_LIBRARY_DRIVER, so it registers nothing and uses whatever
# the library registered: the driver under test is the one that ships.
#
# It exists only when the library was configured with the SQL capability, because
# that is the only configuration in which the library has a SQL driver at all.
# Configure once per mode; the two are not comparable in one build.
if(UFLIB_CAPABILITY_SQL)
    add_executable(ufconfig_sql_live_library
        sql_live_test.c
        ${_live_gen_root}/ufconfig_lookup_generated.c
        ${_live_gen_root}/ufconfig_fielddesc_generated.c
        ${_live_gen_root}/ufconfig_setters_stub_generated.c
    )

    target_include_directories(ufconfig_sql_live_library PRIVATE
        ${_live_gen_root}
        ${CMAKE_SOURCE_DIR}/include
        /opt/include
        /opt/include/mariadb
    )

    target_link_libraries(ufconfig_sql_live_library PRIVATE
        uflib::uflib
        ${MariaDB_LIBRARIES}
        pthread m
    )

    target_compile_definitions(ufconfig_sql_live_library PRIVATE
        UFCONFIG_LIVE_USE_LIBRARY_DRIVER=1
        UFCONFIG_TEST_DIR="${CMAKE_SOURCE_DIR}/tests/ufconfig"
        UFCONFIG_DDL_PATH="${CMAKE_SOURCE_DIR}/src/ufconfig/driver/ufconfig_mariadb.sql"
    )

    ufsrv_apply_build_options(ufconfig_sql_live_library)
endif()
