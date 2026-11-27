# ── Robin Hood table template and its instantiations ─────────────────────────
#
# Plain C programs with their own main() and asserts, not gtest suites: they
# model-check the table against a reference implementation and pin the slot
# layouts the flow table depends on.

function(add_robinhood_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(${name} PRIVATE uflib::uflib)
    ufsrv_apply_build_options(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_robinhood_test(rh_template_tests rh_test.c)

add_robinhood_test(flow4_tests flow4_test.c)

add_robinhood_test(flow6_tests flow6_test.c)

add_robinhood_test(id_64bit_tests id_64bit_test.c)

# Profiling baseline, not a CTest case: the figures in
# dev/technical_designs/UFLIB_ROBINHOOD_HASHTABLE_DESIGN.md come from this binary
# built with -O2. A Debug tree produces numbers that mean nothing.
add_executable(rh_bench rh_bench.c)
target_include_directories(rh_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(rh_bench PRIVATE uflib::uflib)
ufsrv_apply_build_options(rh_bench)
