# ── SQL round-trip check ──────────────────────────────────────────────────
#
# Two halves, deliberately in different languages:
#
#   ufconfig_sql_load     loads a document into MariaDB through the library's
#                         own SQL driver
#   sql_compare.py        reads the rows back through the server's client and
#                         diffs them against the same document
#
# Nothing here is a CTest gate: it needs a reachable MariaDB and the database
# the deployment uses.  Run it by hand:
#
#   ./ufconfig_sql_load "host=…;port=…;user=…;password=…;database=…" <doc> <ns>
#   python3 sql_compare.py --document <doc> --namespace <ns> --pairs-written N
#
# The loader needs the library built with the SQL capability, since the point is to
# exercise the driver that ships rather than one written for a test.

if(NOT UFLIB_CAPABILITY_SQL)
    message(STATUS "Config module: UFLIB_CAPABILITY_SQL is OFF — ufconfig_sql_load not built")
    return()
endif()

set(_rt_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)

set(_rt_schema_dir ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/schema)
set(_rt_schema_files
    ${_rt_schema_dir}/core_schema.lua
    ${_rt_schema_dir}/adjunct_schema.lua
    ${_rt_schema_dir}/test_mutable.lua
)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_rt_gen_root}/ufconfig_lookup_generated.c
        ${_rt_gen_root}/ufconfig_fielddesc_generated.c
        ${_rt_gen_root}/ufconfig_setters_stub_generated.c
        ${_rt_gen_root}/ufconfig_fields_generated.h
        ${_rt_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_rt_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_rt_schema_files}
                -o ${_rt_gen_root}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_rt_schema_files}
    COMMENT "Generating the round-trip harness's config artefacts"
    VERBATIM
)

add_executable(ufconfig_sql_load
    sql_load.c
    ${_rt_gen_root}/ufconfig_lookup_generated.c
    ${_rt_gen_root}/ufconfig_fielddesc_generated.c
    ${_rt_gen_root}/ufconfig_setters_stub_generated.c
)

target_include_directories(ufconfig_sql_load PRIVATE
    ${_rt_gen_root}
    ${CMAKE_SOURCE_DIR}/include
    /opt/include
    /opt/include/mariadb
)

target_link_libraries(ufconfig_sql_load PRIVATE uflib::uflib pthread m)

target_compile_definitions(ufconfig_sql_load PRIVATE
    UFCONFIG_DDL_PATH="${CMAKE_SOURCE_DIR}/src/ufconfig/driver/ufconfig_mariadb.sql"
)

ufsrv_apply_build_options(ufconfig_sql_load)

# The reverse direction: the store is the origin, and every serialised form is
# written out for sql_check_dump.py to diff against the store and the document.
add_executable(ufconfig_sql_dump
    sql_dump.c
    ${_rt_gen_root}/ufconfig_lookup_generated.c
    ${_rt_gen_root}/ufconfig_fielddesc_generated.c
    ${_rt_gen_root}/ufconfig_setters_stub_generated.c
)

target_include_directories(ufconfig_sql_dump PRIVATE
    ${_rt_gen_root}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ufconfig_sql_dump PRIVATE uflib::uflib pthread m)

ufsrv_apply_build_options(ufconfig_sql_dump)

# The comparison half is a script, not a build artefact; this target is the
# whole check, in order, against a store the caller names.
add_custom_target(sql_roundtrip
    COMMAND ${CMAKE_COMMAND} -E echo "Run it by hand, in two steps:"
    COMMAND ${CMAKE_COMMAND} -E echo "  $<TARGET_FILE:ufconfig_sql_load> <dsn> <document> <namespace>"
    COMMAND ${CMAKE_COMMAND} -E echo "  ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sql_compare.py --document <document> --namespace <namespace>"
    DEPENDS ufconfig_sql_load
    VERBATIM
)
