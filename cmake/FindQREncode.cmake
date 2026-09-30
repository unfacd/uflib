# FindQREncode.cmake — locate the system libqrencode.
#
# A find-module rather than a package lookup, because libqrencode ships no CMake
# package: there is no QREncodeConfig.cmake or libqrencode-config.cmake, only a
# pkg-config file (libqrencode.pc) and the shared object.  Supplying this module
# is what lets a caller write find_package(QREncode) and get the usual result
# variables.
#
# Result variables:
#
#   QREncode_FOUND         TRUE when both the header and the library were found
#   QRENCODE_INCLUDE_DIRS  include directories, for target_include_directories
#   QRENCODE_LIBRARIES     the library, for target_link_libraries
#   QRENCODE_VERSION       the library's version, when its pkg-config file says so
#
# The name is spelled the upstream way in the package arguments and the uflib
# way in the result variables, matching FindMariaDB.cmake's
# `find_package(MariaDB)` / `MariaDB_LIBRARIES` split.
#
# Unlike MariaDB there is no private prefix to search — the library lives on the
# default paths — so this is a single find_path/find_library pair rather than
# that module's search-then-fall-back structure.
#
# The version is read from libqrencode.pc's Version field, and the reason is
# that nothing better exists.  qrencode.h carries no QRENCODE_VERSION_* block
# (its only #define is the include guard) and the package installs no
# qrencode-config tool for the mariadb_config fallback to use, so the
# pkg-config file's metadata is the only place upstream states a version at
# all.  Reading it as text keeps uflib free of a pkg-config dependency; if the
# file is absent — a hand-built or stripped install — QRENCODE_VERSION is left
# empty, and find_package_handle_standard_args simply reports no version rather
# than failing the lookup over metadata the build does not need.
#
# libqrencode is distributed as a shared object on this fleet: there is no
# libqrencode.a, and upstream's own build produces one only when configured for
# it.  A static archive found here would be a static-archive link-order problem
# of the kind tests/CMakeLists.txt documents; nothing in this tree arranges for
# that case, so it is a gap rather than a handled path.

find_path(QRENCODE_INCLUDE_DIR
    NAMES qrencode.h
)

find_library(QRENCODE_LIBRARY
    NAMES qrencode
)

if(QRENCODE_LIBRARY AND NOT QRENCODE_VERSION)
    # Derived from the library's own directory rather than searched for: the
    # pkg-config file sits beside the archive it describes by construction, and
    # `pkgconfig` is not one of CMake's default search suffixes, so a find_file
    # would need the suffix spelling out and would still be guessing at a layout
    # the located library has already told us.
    get_filename_component(_qrencode_libdir "${QRENCODE_LIBRARY}" DIRECTORY)
    set(QRENCODE_PKGCONFIG_FILE "${_qrencode_libdir}/pkgconfig/libqrencode.pc")

    if(EXISTS "${QRENCODE_PKGCONFIG_FILE}")
        file(STRINGS "${QRENCODE_PKGCONFIG_FILE}" _qrencode_pc_version
            REGEX "^Version:[ \t]*[0-9]+\\.[0-9]+")
        string(REGEX REPLACE "^Version:[ \t]*" "" QRENCODE_VERSION "${_qrencode_pc_version}")
        string(STRIP "${QRENCODE_VERSION}" QRENCODE_VERSION)
    else()
        unset(QRENCODE_PKGCONFIG_FILE)
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(QREncode
    REQUIRED_VARS QRENCODE_LIBRARY QRENCODE_INCLUDE_DIR
    VERSION_VAR QRENCODE_VERSION
)

if(QREncode_FOUND)
    set(QRENCODE_LIBRARIES "${QRENCODE_LIBRARY}")
    set(QRENCODE_INCLUDE_DIRS "${QRENCODE_INCLUDE_DIR}")
endif()

mark_as_advanced(
    QRENCODE_INCLUDE_DIR
    QRENCODE_LIBRARY
    QRENCODE_PKGCONFIG_FILE
)
