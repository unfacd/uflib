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
# A private-prefix install is reached by setting MariaDB_ROOT.  Nothing is
# searched positionally, so a declared prefix and a distro package take the
# same code path and neither is a silent fallback for the other.
#
# Note the two include roots, which are not interchangeable and have both been
# got wrong in this tree before:
#
#   -I<prefix>/include         resolves <mariadb/mysql.h>   (src/db/db_mysql.c)
#   -I<prefix>/include/mariadb resolves <mysql.h>           (src/db/db_abstraction.c)
#
# Both are returned, because both are in use.  The second is the first's own
# mariadb/ subdirectory, so it is derived from it rather than searched again.

set(_MariaDB_include_hints)
if(MariaDB_ROOT)
    list(APPEND _MariaDB_include_hints "${MariaDB_ROOT}/include")
endif()

find_path(MariaDB_INCLUDE_DIR
    NAMES mariadb/mysql.h
    HINTS ${_MariaDB_include_hints}
)

if(MariaDB_INCLUDE_DIR AND EXISTS "${MariaDB_INCLUDE_DIR}/mariadb/mysql.h")
    set(MariaDB_INCLUDE_DIR_LEGACY "${MariaDB_INCLUDE_DIR}/mariadb")
endif()

set(_MariaDB_library_hints)
if(MariaDB_ROOT)
    list(APPEND _MariaDB_library_hints "${MariaDB_ROOT}/lib/mariadb")
endif()

find_library(MariaDB_LIBRARY
    NAMES mariadb mariadbclient mysqlclient
    HINTS ${_MariaDB_library_hints}
)

# The connector carries its own compiler-flags tool; use it for the version
# rather than parsing a header, so a version that disagrees with the library
# cannot be reported.
if(MariaDB_LIBRARY AND NOT MariaDB_VERSION)
    set(_MariaDB_program_hints)
    if(MariaDB_ROOT)
        list(APPEND _MariaDB_program_hints "${MariaDB_ROOT}/bin")
    endif()
    find_program(MariaDB_CONFIG_EXECUTABLE
        NAMES mariadb_config mariadb-config
        HINTS ${_MariaDB_program_hints}
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
    MariaDB_ROOT
    MariaDB_INCLUDE_DIR
    MariaDB_INCLUDE_DIR_LEGACY
    MariaDB_LIBRARY
    MariaDB_CONFIG_EXECUTABLE
)
