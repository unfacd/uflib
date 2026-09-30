#
# tests/ufconfig/secrets/CMakeLists.txt — the encrypted-secret suite.
#
# A subdirectory of its own rather than another file in the parent suite, and
# not by preference: the config module resolves a secrets file whenever the
# *schema* declares an encrypted field, and every binary in the parent suite
# shares one schema.  Adding an encrypted field there would make all six of them
# require a secrets file in their working directory.  This suite therefore runs
# against its own schema and its own generated tree, which is the same reason
# negative/, validate/, coverage/ and serialise/ have theirs.
#
# The fixtures are not hand-written.  The envelopes in fixtures/*.lua were
# produced by ufsrvcorelib's encrypt_plaintext.py, which is the reference
# producer for this scheme, and the keys in fixtures/ufconfig.secrets are the
# keys it used.  That is what makes the round-trip case a compatibility test
# rather than a test that this code agrees with itself.
#

set(_sec_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
set(_sec_schema   ${CMAKE_CURRENT_SOURCE_DIR}/schema_secrets.lua)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_sec_gen_root}/ufconfig_lookup_generated.c
        ${_sec_gen_root}/ufconfig_fielddesc_generated.c
        ${_sec_gen_root}/ufconfig_setters_stub_generated.c
        ${_sec_gen_root}/ufconfig_fields_generated.h
        ${_sec_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_sec_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen
                ${_sec_schema} -o ${_sec_gen_root}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen ${_sec_schema}
    COMMENT "Generating the secrets suite's schema artefacts"
    VERBATIM
)

set(_sec_gen_sources
    ${_sec_gen_root}/ufconfig_lookup_generated.c
    ${_sec_gen_root}/ufconfig_fielddesc_generated.c
    ${_sec_gen_root}/ufconfig_setters_stub_generated.c)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/ufconfig_secrets_paths.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/ufconfig_secrets_paths.h
    @ONLY
)

add_c_test(
    NAME ufconfig_secrets_tests
    TEST_SOURCE ufconfig_secrets_tests.cpp
    C_SOURCES ${_sec_gen_sources}
    INCLUDE_DIRS
        ${_sec_gen_root}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_BINARY_DIR}
    LIBRARIES uflib::uflib pthread
)
