# ── Caller-supplied substitution ──────────────────────────────────────────
#
# varsub resolves $$name against UfConfigLoadOptions.subst_get, so the cases
# here are about the seam: what the callback sees, when it is consulted, what
# happens when it refuses, and the properties the representation must keep --
# raw stays the document's text, and the digest does not depend on what any
# process resolved.

set(_subst_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
set(_subst_schema   ${CMAKE_CURRENT_SOURCE_DIR}/schema_subst.lua)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_subst_gen_root}/ufconfig_lookup_generated.c
        ${_subst_gen_root}/ufconfig_fielddesc_generated.c
        ${_subst_gen_root}/ufconfig_setters_stub_generated.c
        ${_subst_gen_root}/ufconfig_fields_generated.h
        ${_subst_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_subst_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_subst_schema} -o ${_subst_gen_root}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_subst_schema}
    COMMENT "Generating the substitution schema's artefacts"
    VERBATIM
)

set(_subst_gen_sources
    ${_subst_gen_root}/ufconfig_lookup_generated.c
    ${_subst_gen_root}/ufconfig_fielddesc_generated.c
    ${_subst_gen_root}/ufconfig_setters_stub_generated.c)

add_c_test(
    NAME ufconfig_subst_tests
    TEST_SOURCE ufconfig_subst_tests.cpp
    C_SOURCES ${_subst_gen_sources}
    INCLUDE_DIRS ${_subst_gen_root} ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}
    LIBRARIES uflib::uflib pthread
)
