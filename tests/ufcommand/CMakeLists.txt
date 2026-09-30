# ── ufcommand ─────────────────────────────────────────────────────────────
# The dispatch/alias suite is an ordinary gtest binary.
#
# The memory-safety suite forks a child per case: under AddressSanitizer a
# use-after-free or an out-of-bounds access aborts the process, which would
# otherwise take the whole binary down and hide every later result.

add_c_test(
    NAME ufcommand_tests
    TEST_SOURCE ufcommand_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

add_c_test(
    NAME ufcommand_mem_safety_tests
    TEST_SOURCE ufcommand_mem_safety.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib pthread
)

# ── Standalone stress test executable ─────────────────────────────────────
# Not registered with CTest (takes --iterations/--seed/--keys); build only.

add_executable(ufcommand_stress
    ufcommand_stress.c
)
target_include_directories(ufcommand_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(ufcommand_stress PRIVATE
    uflib::uflib pthread m
)
ufsrv_apply_build_options(ufcommand_stress)

# ── Telemetry ─────────────────────────────────────────────────────────────
# The observer is optional and advisory, so this is a plain CTest program rather
# than a gtest suite.  It is a WHITE-BOX test: the counters are private, so it
# includes the module's private header and queries them directly, then exercises
# the single public call.

add_executable(ufcommand_telemetry_tests ufcommand_telemetry_tests.c)
target_include_directories(ufcommand_telemetry_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/ufcommand
)
target_link_libraries(ufcommand_telemetry_tests PRIVATE uflib::uflib m)
ufsrv_apply_build_options(ufcommand_telemetry_tests)
add_test(NAME ufcommand_telemetry_tests COMMAND ufcommand_telemetry_tests)

# ── Predictive completion ─────────────────────────────────────────────────
# Adversarial rather than behavioural: the completion seam takes host callbacks,
# so its cases are chosen to break it -- hostile inputs, malformed candidates,
# providers that mutate the registry underneath the engine.  Like the telemetry
# program it is a plain CTest target rather than a gtest suite: `add_c_test`
# links gtest_shared_main (which defines main) and forces a C++ linker, so a C
# program with its own main() cannot use it.  It reads the telemetry counters
# through the module's private header, hence the extra include directory.

add_executable(ufcommand_completion_tests ufcommand_completion_tests.c)
target_include_directories(ufcommand_completion_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/ufcommand
)
target_link_libraries(ufcommand_completion_tests PRIVATE uflib::uflib m)
ufsrv_apply_build_options(ufcommand_completion_tests)
add_test(NAME ufcommand_completion_tests COMMAND ufcommand_completion_tests)

# ── Predictive-completion golden oracle ───────────────────────────────────
# The black-box counterpart to the adversarial suite above: a fixed corpus of
# typed lines and a cursor, compared byte-for-byte against a checked-in expected
# record, using the public headers alone.  It carries no `src/ufcommand` include
# path because it must not reach past them — the oracle asserts what a host can
# see, which is also why the record holds the candidate order and not the score.
#
# The fixtures are passed as absolute paths for the same reason as ufcommand_e2e:
# the driver's own defaults would resolve relative to this directory.

add_executable(ufcommand_completion_golden ufcommand_completion_golden.c)
target_include_directories(ufcommand_completion_golden PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ufcommand_completion_golden PRIVATE uflib::uflib m)
ufsrv_apply_build_options(ufcommand_completion_golden)
add_test(NAME ufcommand_completion_golden
    COMMAND ufcommand_completion_golden
            ${CMAKE_CURRENT_SOURCE_DIR}/golden_completion.in
            ${CMAKE_CURRENT_SOURCE_DIR}/golden_completion.out
)

# ── Microbenchmark ────────────────────────────────────────────────────────
# Produces every figure in the design document's Performance section, so the
# numbers can be regenerated rather than taken on trust.  Built with the normal
# build options, which is what makes it comparable across changes — but note
# that a sanitizer build measures the sanitizer, so run it from a release or
# debug preset without sanitizers, and take a median of three.

add_executable(ufcommand_bench
    ufcommand_bench.c
)
target_include_directories(ufcommand_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ufcommand_bench PRIVATE
    uflib::uflib m
)
ufsrv_apply_build_options(ufcommand_bench)

# ── End-to-end golden corpus ──────────────────────────────────────────────
# An exact-oracle integration harness rather than a gtest binary, so it is
# registered directly instead of through add_c_test().  It builds three
# registries -- fixture only, fixture plus the user configuration, and a
# save/load round-trip of that configuration -- and compares each case's
# canonical result record byte-for-byte against golden_e2e.tsv.
#
# The oracle and the config fixture are passed as absolute paths because the
# driver's own defaults are relative to its package root, so invoking it from
# anywhere else silently looks for the wrong file.

add_executable(ufcommand_e2e
    ufcommand_e2e.c
)
target_include_directories(ufcommand_e2e PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ufcommand_e2e PRIVATE
    uflib::uflib m
)
ufsrv_apply_build_options(ufcommand_e2e)
add_test(NAME ufcommand_e2e
    COMMAND ufcommand_e2e
            ${CMAKE_CURRENT_SOURCE_DIR}/golden_e2e.tsv
            ${CMAKE_CURRENT_SOURCE_DIR}/golden_e2e_config.lua
)

# ── Remediation regressions ───────────────────────────────────────────────
# Two self-contained C programs carried over from the module's remediation pass.
# test_remediation.c asserts the previously-reported defect classes directly --
# including a *chained* alias whose second expansion round carries $n, which is
# the shape that used to double-free.  test_ufcommand.c is the older, simpler
# smoke corpus and needs its two golden files passed by absolute path.

add_executable(ufcommand_remediation_tests test_remediation.c)
target_include_directories(ufcommand_remediation_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ufcommand_remediation_tests PRIVATE uflib::uflib m)
ufsrv_apply_build_options(ufcommand_remediation_tests)
add_test(NAME ufcommand_remediation_tests COMMAND ufcommand_remediation_tests)

add_executable(ufcommand_smoke_tests test_ufcommand.c)
target_include_directories(ufcommand_smoke_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ufcommand_smoke_tests PRIVATE uflib::uflib m)
ufsrv_apply_build_options(ufcommand_smoke_tests)
add_test(NAME ufcommand_smoke_tests
    COMMAND ufcommand_smoke_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/golden_commands.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/golden_user_config.lua
)

