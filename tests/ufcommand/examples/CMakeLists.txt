# ── ufcommand sample host application ─────────────────────────────────────
# A consumer of the module, not a test: it is not registered with CTest, so a
# failure here is not a red suite.  Build it to see the documented integration
# flow end to end.

add_executable(ufcommand_host_app
    ufcommand_host_app.c
)
target_include_directories(ufcommand_host_app PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(ufcommand_host_app PRIVATE
    uflib::uflib m
)
ufsrv_apply_build_options(ufcommand_host_app)
