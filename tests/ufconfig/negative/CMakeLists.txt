# ── Schema rejection ──────────────────────────────────────────────────────
#
# The positive tests prove valid documents load.  This proves invalid ones do
# not — one case per rule the loader can enforce, each asserting the specific
# status that rule produces rather than an either/or that would pass whichever
# fired.
#
# Not a CTest gate: it prints, and the printed lines are the evidence.

set(_neg_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
# flat: headers and sources side by side, as the generator emits them
set(_neg_schema   ${CMAKE_CURRENT_SOURCE_DIR}/schema_negative.lua)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_neg_gen_root}/ufconfig_lookup_generated.c
        ${_neg_gen_root}/ufconfig_fielddesc_generated.c
        ${_neg_gen_root}/ufconfig_setters_stub_generated.c
        ${_neg_gen_root}/ufconfig_fields_generated.h
        ${_neg_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_neg_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_neg_schema} -o ${_neg_gen_root}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_neg_schema}
    COMMENT "Generating the negative schema's artefacts"
    VERBATIM
)

add_executable(ufconfig_negative
    negative_test.c
    ${_neg_gen_root}/ufconfig_lookup_generated.c
    ${_neg_gen_root}/ufconfig_fielddesc_generated.c
    ${_neg_gen_root}/ufconfig_setters_stub_generated.c
)
target_include_directories(ufconfig_negative PRIVATE
    ${_neg_gen_root} ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ufconfig_negative PRIVATE uflib::uflib pthread m)
ufsrv_apply_build_options(ufconfig_negative)
