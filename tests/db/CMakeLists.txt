if(NOT UFLIB_CAPABILITY_SQLITE)
    return()
endif()

add_c_test(
    NAME db_sqlite_tests
    TEST_SOURCE db_sqlite_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}
        ${SQLite3_INCLUDE_DIRS}
    LIBRARIES uflib::uflib SQLite3::SQLite3
)

add_c_test(
    NAME db_sqlite_migration_tests
    TEST_SOURCE db_sqlite_migration_tests.cpp
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/uflib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}
        ${SQLite3_INCLUDE_DIRS}
    LIBRARIES uflib::uflib SQLite3::SQLite3
)
