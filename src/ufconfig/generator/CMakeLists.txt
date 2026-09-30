#
# src/ufconfig/generator/CMakeLists.txt — the config generation pipeline.
#
# This is a SEPARATE PROJECT, deliberately.  It is not added to uflib's build by
# add_subdirectory() anywhere, and uflib's own CMakeLists.txt contains no
# reference to it.  Run it explicitly, from the repository root:
#
#     cmake -S src/ufconfig/generator -B build/ufconfig-gen
#     cmake --build build/ufconfig-gen
#
# Why it is separate
# ------------------
# The config module has two modes and only one of them belongs to a library
# build:
#
#   Generation  — run once, when the schema changes.  Turns the schema into the
#                 field tables.  Needs a Python interpreter.  This project.
#
#   Runtime     — the module compiled into uflib and reached through its public
#                 interface.  Needs nothing but a C compiler.  uflib's build.
#
# Keeping them apart is what lets a consumer build uflib without Python
# installed, and it is what stops a schema edit from silently rewriting what an
# already-built library contains.  A schema change is a deliberate act that
# produces a reviewable diff of the generated files; it is not a side effect of
# running a build.
#
# The artefacts are committed
# ---------------------------
# The output is a pure function of the schema, which is committed, so the
# generated files are committed alongside it.  The consequence is the point: a
# clean checkout of uflib builds with no Python present.  Re-run this project
# only when a schema file changes.  Regeneration is idempotent — the banner
# carries a hash of the schema, not a build time — so an unchanged schema
# produces a byte-identical file and a schema change produces a reviewable diff.
#
# Every name below is declared once
# ---------------------------------
# No path or file name appears as a literal past the declarations block.  The
# layout is stated once, in one place, so that moving a directory or renaming a
# file is a one-line edit here rather than a search-and-replace through the
# body — and so that the whole shape of what this project reads and writes can
# be read at a glance.
#

cmake_minimum_required(VERSION 3.20)

project(ufconfig_generator LANGUAGES NONE)

# ── Declarations ─────────────────────────────────────────────────────────────
# Locations, relative to the repository root.  The generator's own directory
# and the module directory above it are not listed here — they are reached by
# walking up from this file, which is what the anchor block below does.
set(_rel_generated_dir  tests/ufconfig/generated)   # the pipeline's own tree; consumers generate their own
set(_rel_sample_dir     src/ufconfig/sample)
set(_rel_public_inc     include/uflib/ufconfig)

# Directory names beneath the generator.
set(_dn_grammar         grammer)
set(_dn_schema          schema)

# Tool names.
set(_fn_generator       ufconfig-gen)
set(_fn_verifier        ufconfig-verify)

# Inputs — the syntax specification.  Not read by the generator; see the note
# where these are checked.
set(_fn_grammar         ufconfig.ebnf
                        ufconfig.abnf)

# Inputs — the schema, in merge order: core first, then the adjuncts that
# extend it.  Adding a tier means adding it here.
set(_fn_schema          core_schema.lua
                        adjunct_schema.lua
                        test_mutable.lua)

# Inputs — sample documents the schema is verified against.
set(_fn_sample          sample.config.lua)

# Outputs — implementation.  Never installed.
set(_fn_private         ufconfig_lookup_generated.c
                        ufconfig_fielddesc_generated.c
                        ufconfig_setters_stub_generated.c)

# Outputs — the public contract.  A caller indexes fields by identifier and
# writes mutable ones through the generated setters, so these two are installed
# beside the handwritten public headers.
set(_fn_public          ufconfig_fields_generated.h
                        ufconfig_setters_stub_generated.h)

# ── Anchors ──────────────────────────────────────────────────────────────────
# The generator directory is where this file sits.  The two above it and the
# repository root are reached by walking up one component at a time.
#
# Walking up rather than writing `../../..` is not a style choice: cmake_path
# leaves a trailing separator on the result when the component it removes is
# `..`, so a root obtained that way would put a double slash into every path
# derived from it — in the command lines, the dependency file, and the summary
# printed at configure time.  PARENT_PATH returns a clean path.
#
# Nothing here resolves symlinks, so the paths reported and written are the ones
# the invoking tree names rather than wherever the filesystem physically keeps
# it.

set(_generator_dir ${CMAKE_CURRENT_SOURCE_DIR})
cmake_path(NORMAL_PATH _generator_dir)
cmake_path(GET _generator_dir PARENT_PATH _module_dir)
cmake_path(GET _module_dir    PARENT_PATH _src_dir)
cmake_path(GET _src_dir       PARENT_PATH _repo_root)

# ── Absolute locations ───────────────────────────────────────────────────────
# Built from an anchor above and a declaration above that, never restated.

set(_generated_dir      ${_repo_root}/${_rel_generated_dir})
set(_sample_dir         ${_repo_root}/${_rel_sample_dir})
set(_public_dir         ${_repo_root}/${_rel_public_inc})

set(_grammar_dir        ${_generator_dir}/${_dn_grammar})
set(_schema_dir         ${_generator_dir}/${_dn_schema})

set(_generator_tool     ${_generator_dir}/${_fn_generator})
set(_verifier_tool      ${_generator_dir}/${_fn_verifier})

# ── Absolute file lists ──────────────────────────────────────────────────────

set(_grammar_files "")
foreach(_f IN LISTS _fn_grammar)
    list(APPEND _grammar_files ${_grammar_dir}/${_f})
endforeach()

set(_schema_files "")
foreach(_f IN LISTS _fn_schema)
    list(APPEND _schema_files ${_schema_dir}/${_f})
endforeach()

set(_sample_files "")
foreach(_f IN LISTS _fn_sample)
    list(APPEND _sample_files ${_sample_dir}/${_f})
endforeach()

set(_private_artefacts "")
foreach(_f IN LISTS _fn_private)
    list(APPEND _private_artefacts ${_generated_dir}/${_f})
endforeach()

set(_public_artefacts "")
foreach(_f IN LISTS _fn_public)
    list(APPEND _public_artefacts ${_public_dir}/${_f})
endforeach()

# ── Presence checks ──────────────────────────────────────────────────────────
# A missing input is reported at configure time, naming the file, rather than
# surfacing later as a generator failure with a line number in a Lua file.

foreach(_f IN LISTS _grammar_files _schema_files _sample_files)
    if(NOT EXISTS ${_f})
        message(FATAL_ERROR "ufconfig generator: input not found: ${_f}")
    endif()
endforeach()

foreach(_f IN LISTS _generator_tool _verifier_tool)
    if(NOT EXISTS ${_f})
        message(FATAL_ERROR "ufconfig generator: tool not found: ${_f}")
    endif()
endforeach()

# ── Interpreter ──────────────────────────────────────────────────────────────
# Needed by this project and by nothing in uflib's own build.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ── Generate ─────────────────────────────────────────────────────────────────
# One invocation writes both destinations.  The split is by audience, not by
# stage: a single pass is what guarantees the two sets cannot disagree.

add_custom_command(
    OUTPUT  ${_private_artefacts} ${_public_artefacts}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_generated_dir} ${_public_dir}
    COMMAND ${Python3_EXECUTABLE} ${_generator_tool} ${_schema_files}
                -o ${_generated_dir}
                --public-outdir ${_public_dir}
    DEPENDS ${_generator_tool} ${_schema_files}
    COMMENT "Generating ufconfig field tables from the schema"
    VERBATIM
)

add_custom_target(generate ALL
    DEPENDS ${_private_artefacts} ${_public_artefacts}
)

# ── Schema report ────────────────────────────────────────────────────────────
# Every field the merged schema declares, one per line.  The same walk the
# generator does, without writing anything — useful when reviewing a schema
# change.

add_custom_target(schema-report
    COMMAND ${Python3_EXECUTABLE} ${_generator_tool} ${_schema_files} --emit-schema-report
    COMMENT "Field table as declared by the schema"
    VERBATIM
)

# ── Verify ───────────────────────────────────────────────────────────────────
# Parses each sample document against the schema.  A document that fails here is
# a sample that no longer matches the schema it claims to follow.

add_custom_target(verify
    COMMAND ${Python3_EXECUTABLE} ${_verifier_tool} ${_sample_files}
    COMMENT "Verifying the sample documents against the schema"
    VERBATIM
)

# ── Summary ──────────────────────────────────────────────────────────────────
# Printed at configure time so a wrong location is visible immediately.

message(STATUS "ufconfig generator")
message(STATUS "  generator : ${_generator_dir}")
message(STATUS "  grammar   : ${_grammar_dir}/")
message(STATUS "  schema    : ${_schema_dir}/")
message(STATUS "  sample    : ${_sample_dir}/")
message(STATUS "  tool      : ${_generator_tool}")
message(STATUS "  private   : ${_generated_dir}/")
message(STATUS "  public    : ${_public_dir}/")
