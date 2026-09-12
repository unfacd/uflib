# check_opaque_boundary.cmake
#
# CTest script for Gate 2.8 / Gate 7.6 (D-8 / HOP-033).
#
# Verifies that sizeof(HopscotchHashTable) FAILS to compile in consumer code.
# The V2 opaque handle must not expose its struct definition through the
# public header — only a forward declaration is visible.  If this test
# succeeds, the opaque boundary has been broken and the build must fail.
#
# Usage (from CTest):
#   add_test(NAME hopscotch_opaque_boundary
#     COMMAND ${CMAKE_COMMAND}
#       -DCOMPILER=${CMAKE_C_COMPILER}
#       -DINCLUDE_DIR=${CMAKE_SOURCE_DIR}/include
#       -DSOURCE_FILE=${CMAKE_SOURCE_DIR}/src/adt/hopscotch_hashtable_v2/try_compile_opaque_boundary.c
#       -P ${CMAKE_SOURCE_DIR}/cmake/check_opaque_boundary.cmake)

if(NOT COMPILER)
    message(FATAL_ERROR "COMPILER not defined — pass -DCOMPILER=<path>")
endif()
if(NOT INCLUDE_DIR)
    message(FATAL_ERROR "INCLUDE_DIR not defined — pass -DINCLUDE_DIR=<path>")
endif()
if(NOT SOURCE_FILE)
    message(FATAL_ERROR "SOURCE_FILE not defined — pass -DSOURCE_FILE=<path>")
endif()

execute_process(
    COMMAND ${COMPILER} -fsyntax-only -I${INCLUDE_DIR} ${SOURCE_FILE}
    RESULT_VARIABLE _result
    OUTPUT_QUIET
    ERROR_VARIABLE _stderr
)

if(_result EQUAL 0)
    message(FATAL_ERROR
        "OPAQUE BOUNDARY VIOLATION: sizeof(HopscotchHashTable) compiled successfully.\n"
        "The opaque handle has been broken — the struct definition in _priv.h is\n"
        "visible to consumer code.  Check that:\n"
        "  1. hopscotch_hashtable_v2_priv.h is NOT included from the public header\n"
        "  2. The struct definition is only in _priv.h (src/, not installed)\n"
        "  3. The public header has only a forward declaration:\n"
        "     typedef struct HopscotchHashTable HopscotchHashTable;\n"
        "\n"
        "Consumer code must NOT be able to take sizeof(HopscotchHashTable).")
else()
    message(STATUS "Opaque boundary intact: sizeof(HopscotchHashTable) correctly fails to compile")
endif()
