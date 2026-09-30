# ── Transform and validator coverage ──────────────────────────────────────
#
# Its own schema and document, not the shipped ones: the shipped schema declares
# one transform out of nine, so exercising the catalogue through it would prove
# almost nothing.  Here every transform and every validator has a field of its
# own and a real value to act on.
#
# Not a CTest gate — it prints, and is read.  Built always.

set(_cov_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
# flat: headers and sources side by side, as the generator emits them
set(_cov_schema   ${CMAKE_CURRENT_SOURCE_DIR}/schema_coverage.lua)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_cov_gen_root}/ufconfig_lookup_generated.c
        ${_cov_gen_root}/ufconfig_fielddesc_generated.c
        ${_cov_gen_root}/ufconfig_setters_stub_generated.c
        ${_cov_gen_root}/ufconfig_fields_generated.h
        ${_cov_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_cov_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_cov_schema} -o ${_cov_gen_root}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_cov_schema}
    COMMENT "Generating the coverage schema's artefacts"
    VERBATIM
)

add_executable(ufconfig_coverage
    coverage_test.c
    ${_cov_gen_root}/ufconfig_lookup_generated.c
    ${_cov_gen_root}/ufconfig_fielddesc_generated.c
    ${_cov_gen_root}/ufconfig_setters_stub_generated.c
)

target_include_directories(ufconfig_coverage PRIVATE
    ${_cov_gen_root} ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ufconfig_coverage PRIVATE uflib::uflib pthread m)
ufsrv_apply_build_options(ufconfig_coverage)

add_custom_target(run_coverage
    COMMAND $<TARGET_FILE:ufconfig_coverage> ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ufconfig_coverage
    COMMENT "Every transform and validator, against real data"
    VERBATIM
)
