# ── mjson extension tests ─────────────────────────────────────────────────
#
# A C program with its own main() and asserts, not a gtest suite: it predates
# the harness and covers mjson_ex.c's wildcard, recursive and filter paths.
# Gated with the parser it exercises.

if(UFLIB_CAPABILITY_MJSON)
    add_executable(mjson_ex_tests test_mjso.c)

    target_include_directories(mjson_ex_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    )

    target_link_libraries(mjson_ex_tests PRIVATE uflib::uflib m)

    ufsrv_apply_build_options(mjson_ex_tests)

    add_test(NAME mjson_ex_tests COMMAND mjson_ex_tests)
endif()
