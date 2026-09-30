# ── Standalone validator ──────────────────────────────────────────────────
#
# A host application in miniature: builds a descriptor from the generated
# schema, injects it, loads the file the caller names.  Nothing stubbed.
#
#   ufconfig-validate --config-file=<path>
#   ufconfig-validate --table=cases/expected.tsv --cases=cases
#
# The truth table is the deliverable: a config file whose result is not written
# down is one whose result nobody has agreed on.

set(_val_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
# flat: headers and sources side by side, as the generator emits them
set(_val_schema   ${CMAKE_CURRENT_SOURCE_DIR}/schema_full.lua)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_val_gen_root}/ufconfig_lookup_generated.c
        ${_val_gen_root}/ufconfig_fielddesc_generated.c
        ${_val_gen_root}/ufconfig_setters_stub_generated.c
        ${_val_gen_root}/ufconfig_fields_generated.h
        ${_val_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_val_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_val_schema} -o ${_val_gen_root}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_val_schema}
    COMMENT "Generating the validator's schema artefacts"
    VERBATIM
)

# The named-getter caller, reconstructed from the generated header.  Generated
# rather than handwritten so it cannot fall behind the schema: a new field
# produces a new accessor, and the next build adds a call for it.
set(_val_named_getters ${_val_gen_root}/named_getters.c)
add_custom_command(
    OUTPUT ${_val_named_getters}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/make_named_getters.py
                ${_val_gen_root}/ufconfig_fields_generated.h
                ${_val_named_getters}
                schema_full.lua
    DEPENDS ${_val_gen_root}/ufconfig_fields_generated.h
            ${CMAKE_CURRENT_SOURCE_DIR}/make_named_getters.py
    COMMENT "Reconstructing the named getters from the generated header"
    VERBATIM
)

add_executable(ufconfig_validate
    ufconfig_validate.c
    ${_val_named_getters}
    ${_val_gen_root}/ufconfig_lookup_generated.c
    ${_val_gen_root}/ufconfig_fielddesc_generated.c
    ${_val_gen_root}/ufconfig_setters_stub_generated.c
)
target_include_directories(ufconfig_validate PRIVATE
    ${_val_gen_root} ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ufconfig_validate PRIVATE uflib::uflib pthread m)
ufsrv_apply_build_options(ufconfig_validate)

add_custom_target(run_validate
    COMMAND $<TARGET_FILE:ufconfig_validate>
            --table=${CMAKE_CURRENT_SOURCE_DIR}/cases/expected.tsv
            --cases=${CMAKE_CURRENT_SOURCE_DIR}/cases
    DEPENDS ufconfig_validate
    COMMENT "Every config file against its expected result"
    VERBATIM
)
