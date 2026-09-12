# ── Exponential backoff module tests ─────────────────────────────────────────
#
# READ dev/technical_designs/UFLIB_TESTING_MARIADB.md for more testing options, including layer3
#

add_c_test(
    NAME backoff_algorithm_tests
    TEST_SOURCE backoff_algorithm_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES uflib::uflib
)

# ── Whitebox DB-reconnection test (mocks DB I/O via linker --wrap) ─────────

find_library(MARIADB_LIBRARY NAMES mariadb PATHS /opt/lib/mariadb NO_DEFAULT_PATH)

add_c_test(
    NAME db_reconnect_tests
    TEST_SOURCE db_reconnect_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        /opt/include
        /opt/include/mariadb
    LIBRARIES uflib::uflib ${MARIADB_LIBRARY}
)
target_link_options(db_reconnect_tests PRIVATE
    "-Wl,--wrap=h_execute_query_mariadb"
    "-Wl,--wrap=h_reconnect_mariadb"
)

# ── Standalone stress executable (build only, not registered with CTest) ──

add_executable(backoff_algorithm_stress
    backoff_algorithm_stress.c
)
target_include_directories(backoff_algorithm_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(backoff_algorithm_stress PRIVATE
    uflib::uflib m pthread
)
ufsrv_apply_build_options(backoff_algorithm_stress)

# ── Standalone Toxiproxy integration driver (build only, not in CTest) ─────
# Live-infrastructure probe; run via toxiproxy_reconnect.sh (see
# dev/technical_designs/UFLIB_TESTING_MARIADB.md §7).

add_executable(db_reconnect_driver
    db_reconnect_driver.c
)
target_include_directories(db_reconnect_driver PRIVATE
    ${CMAKE_SOURCE_DIR}/include/uflib
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
    /opt/include
    /opt/include/mariadb
)
target_link_libraries(db_reconnect_driver PRIVATE
    uflib::uflib ${MARIADB_LIBRARY} m pthread
)
ufsrv_apply_build_options(db_reconnect_driver)
