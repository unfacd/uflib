# FindMariaDB.cmake — locate the MariaDB Connector/C.
#
# A find-module rather than a package lookup, because the connector ships no
# CMake package: there is no MariaDBConfig.cmake or mariadb-config.cmake
# anywhere on this fleet, only a pkg-config file (libmariadb.pc) and the
# connector's own mariadb_config tool.  Supplying this module is what lets a
# caller write find_package(MariaDB) and get the usual result variables —
# without it that call finds nothing, which is exactly what it did before this
# file existed.
#
# Result variables:
#
#   MariaDB_FOUND          TRUE when both the header and the library were found
#   MariaDB_INCLUDE_DIRS   include directories, for target_include_directories
#   MariaDB_LIBRARIES      the library, for target_link_libraries
#   MariaDB_VERSION        the connector's version, when mariadb_config says so
#
# The fleet's layout is a private prefix — headers under /opt/include/mariadb
# and the library under /opt/lib/mariadb — which no standard search path knows
# about, so those are searched explicitly and first.  The default paths are
# searched afterwards rather than not at all: a host that installed the
# connector through its package manager should work without a hint, and a
# caller on this fleet should get the fleet's copy.
#
# Note the two include roots, which are not interchangeable and have both been
# got wrong in this tree before:
#
#   -I/opt/include         resolves <mariadb/mysql.h>   (what src/db/db_mysql.c uses)
#   -I/opt/include/mariadb resolves <mysql.h>           (what src/db/db_abstraction.c uses)
#
# Both are returned, because both are in use.

set(_MariaDB_hints /opt/include/mariadb /opt/lib/mariadb)

find_path(MariaDB_INCLUDE_DIR
    NAMES mariadb/mysql.h
    PATHS /opt/include
    NO_DEFAULT_PATH
)
find_path(MariaDB_INCLUDE_DIR
    NAMES mariadb/mysql.h
    PATHS ${_MariaDB_hints}
)

find_path(MariaDB_INCLUDE_DIR_LEGACY
    NAMES mysql.h
    PATHS /opt/include/mariadb
    NO_DEFAULT_PATH
)
find_path(MariaDB_INCLUDE_DIR_LEGACY
    NAMES mysql.h
    PATHS ${_MariaDB_hints}
)

find_library(MariaDB_LIBRARY
    NAMES mariadb mariadbclient mysqlclient
    PATHS /opt/lib/mariadb
    NO_DEFAULT_PATH
)
find_library(MariaDB_LIBRARY
    NAMES mariadb mariadbclient mysqlclient
)

# The connector carries its own compiler-flags tool; use it for the version
# rather than parsing a header, so a version that disagrees with the library
# cannot be reported.
if(MariaDB_LIBRARY AND NOT MariaDB_VERSION)
    find_program(MariaDB_CONFIG_EXECUTABLE
        NAMES mariadb_config mariadb-config
        PATHS /opt/bin
    )
    if(MariaDB_CONFIG_EXECUTABLE)
        execute_process(
            COMMAND ${MariaDB_CONFIG_EXECUTABLE} --version
            OUTPUT_VARIABLE _mariadb_version_out
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" MariaDB_VERSION "${_mariadb_version_out}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MariaDB
    REQUIRED_VARS MariaDB_LIBRARY MariaDB_INCLUDE_DIR
    VERSION_VAR MariaDB_VERSION
)

if(MariaDB_FOUND)
    set(MariaDB_LIBRARIES "${MariaDB_LIBRARY}")
    set(MariaDB_INCLUDE_DIRS "${MariaDB_INCLUDE_DIR}")
    if(MariaDB_INCLUDE_DIR_LEGACY)
        list(APPEND MariaDB_INCLUDE_DIRS "${MariaDB_INCLUDE_DIR_LEGACY}")
    endif()
endif()

mark_as_advanced(
    MariaDB_INCLUDE_DIR
    MariaDB_INCLUDE_DIR_LEGACY
    MariaDB_LIBRARY
    MariaDB_CONFIG_EXECUTABLE
)
