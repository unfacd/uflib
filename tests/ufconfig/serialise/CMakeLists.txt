# ── Serialiser payload tests ──────────────────────────────────────────────
#
# Golden-file comparison: the four serialisers' output against fixed files
# beside them.  The question is payload, not structure -- a serialiser that
# emits perfect JSON with a field missing has failed at the only thing it is
# for, and "is this valid JSON?" would pass it.
#
# Regenerate deliberately, never automatically:
#   ./ufconfig_serialise --regenerate <dir>

set(_ser_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
set(_ser_schema   ${CMAKE_CURRENT_SOURCE_DIR}/schema_serialise.lua)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_ser_gen_root}/ufconfig_lookup_generated.c
        ${_ser_gen_root}/ufconfig_fielddesc_generated.c
        ${_ser_gen_root}/ufconfig_setters_stub_generated.c
        ${_ser_gen_root}/ufconfig_fields_generated.h
        ${_ser_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_ser_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_ser_schema} -o ${_ser_gen_root}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_ser_schema}
    COMMENT "Generating the serialiser tests' schema artefacts"
    VERBATIM
)

add_executable(ufconfig_serialise
    serialise_test.c
    ${_ser_gen_root}/ufconfig_lookup_generated.c
    ${_ser_gen_root}/ufconfig_fielddesc_generated.c
    ${_ser_gen_root}/ufconfig_setters_stub_generated.c
)
target_include_directories(ufconfig_serialise PRIVATE
    ${_ser_gen_root} ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ufconfig_serialise PRIVATE uflib::uflib pthread m)
ufsrv_apply_build_options(ufconfig_serialise)

add_custom_target(run_serialise
    COMMAND $<TARGET_FILE:ufconfig_serialise> ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ufconfig_serialise
    COMMENT "Serialiser output against the golden files"
    VERBATIM
)
