# ── Sample host application ───────────────────────────────────────────────
#
# This is a consumer, not part of uflib.  It exists to demonstrate the boundary
# end to end, and it is built the way a real consumer is built:
#
#   1. It runs the generation pipeline over ITS OWN schema, into ITS OWN build
#      directory.  uflib is not involved and does not know it happened.
#   2. It compiles the generated artefacts into itself.
#   3. It links libuflib and injects the generated symbols at initialisation.
#
# Nothing here is installed and nothing here is a library.  If this directory
# were deleted the uflib build would be unchanged — which is the property the
# arrangement exists to provide.
#
# The schema below is the sample schema the generator ships.  A real
# application carries its own; the pipeline takes whatever paths it is given,
# and the library never sees them.

set(_example_schema_dir ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/schema)
set(_example_generator  ${CMAKE_SOURCE_DIR}/src/ufconfig/generator/ufconfig-gen)

set(_example_schema_files
    ${_example_schema_dir}/core_schema.lua
    ${_example_schema_dir}/adjunct_schema.lua
    ${_example_schema_dir}/test_mutable.lua
)

# The application's own generated tree.  Mirrors the include path so that
# <uflib/ufconfig/ufconfig_fields_generated.h> resolves from here exactly as it
# would from an installed copy.
set(_example_gen_root ${CMAKE_CURRENT_SOURCE_DIR}/generated)
# flat: headers and sources side by side, as the generator emits them

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT
        ${_example_gen_root}/ufconfig_lookup_generated.c
        ${_example_gen_root}/ufconfig_fielddesc_generated.c
        ${_example_gen_root}/ufconfig_setters_stub_generated.c
        ${_example_gen_root}/ufconfig_fields_generated.h
        ${_example_gen_root}/ufconfig_setters_stub_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_example_gen_root}
    COMMAND ${Python3_EXECUTABLE} ${_example_generator} ${_example_schema_files}
                -o ${_example_gen_root}
                
    DEPENDS ${_example_generator} ${_example_schema_files}
    COMMENT "Generating this application's config artefacts from its schema"
    VERBATIM
)

add_executable(ufconfig_host_app
    ufconfig_host_app.c
    ${_example_gen_root}/ufconfig_lookup_generated.c
    ${_example_gen_root}/ufconfig_fielddesc_generated.c
    ${_example_gen_root}/ufconfig_setters_stub_generated.c
)

# The application's own generated include tree comes first: this is where its
# ufconfig_fields_generated.h lives, not uflib's.
target_include_directories(ufconfig_host_app PRIVATE
    ${_example_gen_root}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ufconfig_host_app PRIVATE uflib::uflib pthread)
ufsrv_apply_build_options(ufconfig_host_app)

# A convenience target so the demonstration can be run from the build tree.
add_custom_target(run_host_app
    COMMAND ${CMAKE_COMMAND} -E echo "── sample host application ──"
    COMMAND $<TARGET_FILE:ufconfig_host_app>
            ${CMAKE_CURRENT_SOURCE_DIR}/sample.strict.lua
    DEPENDS ufconfig_host_app
    COMMENT "Running the sample host application against sample.strict.lua"
    VERBATIM
)
