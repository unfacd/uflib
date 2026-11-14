# uflib — C utility library for ufsrv (ADTs, crypto, DB abstraction, networking)
#
cmake_minimum_required(VERSION 3.20)

set(_PROJECT_NAME "uflib")
string(TOUPPER ${_PROJECT_NAME} _PROJECT_NAME_X)

# ── Version extraction ──────────────────────────────────────────────────────

MACRO(getVersionPatchLevel name)
    # Tolerate column-aligned defines, and a trailing \r from a CRLF checkout.
    SET(VERSION_REGEX "^#define[ \t]+${name}[ \t]+(.+)$")
    FILE(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/uflib/version.h"
            VERSION_BIT REGEX ${VERSION_REGEX})
    STRING(REGEX REPLACE ${VERSION_REGEX} "\\1" ${name} "${VERSION_BIT}")
    STRING(STRIP "${${name}}" ${name})
ENDMACRO(getVersionPatchLevel)

getVersionPatchLevel(${_PROJECT_NAME_X}_MAJOR)
getVersionPatchLevel(${_PROJECT_NAME_X}_MINOR)
getVersionPatchLevel(${_PROJECT_NAME_X}_PATCH)
SET(${_PROJECT_NAME_X}_VERSION "${${_PROJECT_NAME_X}_MAJOR}.${${_PROJECT_NAME_X}_MINOR}.${${_PROJECT_NAME_X}_PATCH}")
MESSAGE("Detected ${_PROJECT_NAME_X} version: ${${_PROJECT_NAME_X}_VERSION}")

project(${_PROJECT_NAME}
    VERSION ${${_PROJECT_NAME_X}_VERSION}
    LANGUAGES C ASM
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(BUILD_SHARED_LIBS)
        set(_UFSRV_BUILD_FLAVOUR "SHARED+STATIC")
    else()
        set(_UFSRV_BUILD_FLAVOUR "STATIC")
    endif()
else()
    set(_UFSRV_BUILD_FLAVOUR "OBJECT")
endif()

list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake-modules ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
set(UFSRV_C_WARNING_FLAGS
    -Wall -Wimplicit-fallthrough -Wno-unused-label -Wno-unused-variable
    -Wno-comment -Wno-nullability-completeness -Wno-unknown-attributes)
set(UFSRV_C_DEBUG_FLAGS_DEFAULT -ggdb3)
include(ufsrv_build_options)

add_definitions(-DHAVE_CONFIG_UFLIB_H)

# ── Config generation ────────────────────────────────────────────────────────

# Derived implicitly from CMAKE_BUILD_TYPE — no user-facing option.
#   cmake --preset debug  (or dev / asan / tsan)  → Debug   → 1
#   cmake --preset release                        → Release → 0
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(UF_DEBUG_BUILD 1)
else()
    set(UF_DEBUG_BUILD 0)
endif()

# ── Verbosity levels ─────────────────────────────────────────────────────────
# L3 implies L2, and either implies the base debug build switch.
option(UF_DEBUG_BUILD_L2 "Verbose level-2 debug logging" OFF)
option(UF_DEBUG_BUILD_L3 "Very verbose level-3 debug logging (per-object traces)" OFF)
if(UF_DEBUG_BUILD_L3)
    set(UF_DEBUG_L3 1)
    set(UF_DEBUG_L2 1)
else()
    set(UF_DEBUG_L3 0)
    if(UF_DEBUG_BUILD_L2)
        set(UF_DEBUG_L2 1)
    else()
        set(UF_DEBUG_L2 0)
    endif()
endif()
if(UF_DEBUG_L2 OR UF_DEBUG_L3)
    set(UF_DEBUG_BUILD 1)
endif()
add_compile_definitions(UF_DEBUG_BUILD=${UF_DEBUG_BUILD}
                        UF_DEBUG_BUILD_L2=${UF_DEBUG_L2}
                        UF_DEBUG_BUILD_L3=${UF_DEBUG_L3})

# ── Build-time capabilities ──────────────────────────────────────────────────
#
option(UFLIB_CAPABILITY_HIREDIS "Build the Redis driver for the config module" OFF)
option(UFLIB_CAPABILITY_SQL     "Build the SQL driver (libmariadb / MySQL) for the config module" OFF)
option(UFLIB_CAPABILITY_SQLITE  "Build the SQLite driver for the config module" OFF)
option(UFLIB_CAPABILITY_QUIRC   "Build QR-code reading: the vendored quirc library and src/qr_code/" OFF)
option(UFLIB_CAPABILITY_QRENCODE "Build QR-code rendering, against the system libqrencode" OFF)
option(UFLIB_CAPABILITY_UTF8PROC "Build the utf8proc-backed UTF-8 helpers, against the system libutf8proc" OFF)
option(UFLIB_CAPABILITY_K12        "Vendor the KangarooTwelve hash (packages/K12)" OFF)
option(UFLIB_CAPABILITY_LZF        "Vendor the lzf compression library (packages/lzf)" OFF)
option(UFLIB_CAPABILITY_MJSON      "Vendor the mjson parser (packages/mjson)" OFF)
option(UFLIB_CAPABILITY_PROTOBUF_C "Vendor the protobuf-c runtime (packages/protobuf-c)" OFF)
option(UFLIB_CAPABILITY_ZLOG       "Build the zlog backend for the logger (packages/zlog)" OFF)


# ── Every capability at once ─────────────────────────────────────────────────
# A convenience for the maintainer scripts, which build and install the maximal library.
option(UFLIB_CAPABILITY_ALL "Turn on every capability (maintainer builds)" OFF)

if(UFLIB_CAPABILITY_ALL)
    foreach(_cap HIREDIS SQL QUIRC QRENCODE UTF8PROC
                 K12 LZF MJSON PROTOBUF_C ZLOG)
        set(UFLIB_CAPABILITY_${_cap} ON)
    endforeach()
    message(STATUS "Capabilities: ALL — every capability except SQLITE forced on")
endif()

if(UFLIB_CAPABILITY_SQL)
    find_package(MariaDB REQUIRED)
    message(STATUS "Config module: SQL capability — MariaDB ${MariaDB_VERSION}, ${MariaDB_LIBRARIES}")
endif()

set(UFLIB_PC_REQUIRES_PRIVATE "")

if(UFLIB_CAPABILITY_SQLITE)
    find_package(SQLite3 REQUIRED)
    set(UFLIB_PC_REQUIRES_PRIVATE ", sqlite3")
    message(STATUS "Config module: SQLite capability — SQLite ${SQLite3_VERSION}, exported to consumers")
endif()

set(_uflib_db_enabled OFF)
if(UFLIB_CAPABILITY_SQL OR UFLIB_CAPABILITY_SQLITE)
    set(_uflib_db_enabled ON)
endif()

if(UFLIB_CAPABILITY_QRENCODE)
    find_package(QREncode REQUIRED)
    message(STATUS "QR-code module: rendering capability — libqrencode ${QRENCODE_VERSION}, ${QRENCODE_LIBRARIES}")
endif()

if(UFLIB_CAPABILITY_UTF8PROC)
    find_package(Utf8proc REQUIRED)
    message(STATUS "UTF-8 helpers: utf8proc capability — libutf8proc ${UTF8PROC_VERSION}, ${UTF8PROC_LIBRARIES}")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config_uflib.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/config_uflib.h
    @ONLY
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IS_ROOT_PROJECT ON)
else()
    set(IS_ROOT_PROJECT OFF)
endif()

option(_PACKAGE_TESTS "Enable test targets" OFF)
option(_PACKAGE_INSTALL "Enable install target" ${IS_ROOT_PROJECT})

# ── GTest ──
if(_PACKAGE_TESTS)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    # Share one FetchContent clone across all build directories.
    # Without this each build/ preset downloads its own 20 MB googletest clone.
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/build/_fetchcontent"
        CACHE PATH "Shared FetchContent download directory")
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.0
    )
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# ── Doxygen ──
if(IS_ROOT_PROJECT)
    find_package(Doxygen QUIET)
    if(Doxygen_FOUND)
        include(cmake/doxygen.cmake)
    else()
        message(STATUS "Doxygen not found — 'docs' target disabled.")
    endif()
endif()

# ── Dependencies ──

# OpenSSL — two paths: registry-driven (subtree) or find_package (standalone)
if(NOT IS_ROOT_PROJECT)
    option(DEV_ENVIRONMENT_AA "Dev environment with access to ufsrv provisioning" OFF)
    if(EXISTS "${CMAKE_SOURCE_DIR}/cmake-modules/ufsrv_linked_libraries_registry.cmake")
        include(${CMAKE_SOURCE_DIR}/cmake-modules/ufsrv_linked_libraries_registry.cmake)
    endif()
    if(NOT DEFINED UFSRV_LINKED_LIB_SSL)
        set(UFSRV_LINKED_LIB_SSL SYSTEM)
    endif()
    if(NOT DEFINED UFSRV_LINKED_LIB_CRYPTO)
        set(UFSRV_LINKED_LIB_CRYPTO SYSTEM)
    endif()
endif()

if(NOT IS_ROOT_PROJECT AND UFSRV_LINKED_LIB_SSL STREQUAL "OPT" AND UFSRV_LINKED_LIB_CRYPTO STREQUAL "OPT")
    add_library(ssl SHARED IMPORTED)
    set_target_properties(ssl PROPERTIES IMPORTED_LOCATION "${OPT_PACKAGE_SSL}")
    add_library(crypto SHARED IMPORTED)
    set_target_properties(crypto PROPERTIES IMPORTED_LOCATION "${OPT_PACKAGE_CRYPTO}")
    get_filename_component(_uflib_ssl_libdir "${OPT_PACKAGE_SSL}" DIRECTORY)
    get_filename_component(_uflib_ssl_prefix "${_uflib_ssl_libdir}" DIRECTORY)
    set(OPENSSL_INCLUDE_DIR "${_uflib_ssl_prefix}/include" CACHE PATH "OpenSSL include directory")
    message(STATUS "OpenSSL via OPT library package: ${OPT_PACKAGE_SSL}")
else()
    find_package(OpenSSL REQUIRED)
    if(OPENSSL_FOUND)
        message(STATUS "Found OpenSSL ${OPENSSL_VERSION}")
    else()
        message(FATAL_ERROR "OpenSSL Not Found")
    endif()
endif()

# ── Valgrind (optional — standard_valgrind_includes.h conditionally includes it) ──────

find_path(VALGRIND_INCLUDE_DIR valgrind/valgrind.h
    HINTS ${VALGRIND_ROOT}/include /usr/include /usr/local/include
)
if(VALGRIND_INCLUDE_DIR)
    message(STATUS "Found valgrind headers: ${VALGRIND_INCLUDE_DIR}")
else()
    message(STATUS "Valgrind headers not found — valgrind-dependent features will be unavailable")
endif()

# ── Build target — OBJECT library (dual-mode) ────────────────────────────────

add_library(${_PROJECT_NAME} OBJECT)

if(IS_ROOT_PROJECT)
    add_library(${_PROJECT_NAME}_static STATIC $<TARGET_OBJECTS:${_PROJECT_NAME}>)
    set_target_properties(${_PROJECT_NAME}_static PROPERTIES OUTPUT_NAME ${_PROJECT_NAME})

    if(BUILD_SHARED_LIBS)
        # Interim switch: until every public symbol carries PUBLIC_API, exporting
        # all globals keeps the shared library usable by consumers. Set OFF to
        # enforce the annotation sweep; a gap then fails at link time.
        option(UFLIB_SHARED_EXPORT_ALL
            "Export every global from the shared library (interim; disable once PUBLIC_API coverage is complete)" ON)

        if(UFLIB_SHARED_EXPORT_ALL)
            set(_UFLIB_SHARED_VISIBILITY default)
        else()
            set(_UFLIB_SHARED_VISIBILITY hidden)
        endif()

        # Sources compile once, in the OBJECT target, so visibility must be set
        # there; a preset on the shared target would be inert (it compiles nothing).
        set_target_properties(${_PROJECT_NAME} PROPERTIES
            C_VISIBILITY_PRESET ${_UFLIB_SHARED_VISIBILITY}
            VISIBILITY_INLINES_HIDDEN ON)

        add_library(${_PROJECT_NAME}_shared SHARED $<TARGET_OBJECTS:${_PROJECT_NAME}>)
        set_target_properties(${_PROJECT_NAME}_shared PROPERTIES
            OUTPUT_NAME ${_PROJECT_NAME}
            VERSION   ${${_PROJECT_NAME_X}_VERSION}
            SOVERSION ${${_PROJECT_NAME_X}_MAJOR})
        add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME}_shared)
    else()
        add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME}_static)
    endif()
else()
    add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME})
endif()

ufsrv_apply_build_options(${_PROJECT_NAME})
ufsrv_build_summary(${_PROJECT_NAME})
if(IS_ROOT_PROJECT)
    ufsrv_apply_build_options(${_PROJECT_NAME}_static)
    ufsrv_build_summary(${_PROJECT_NAME}_static)
    if(BUILD_SHARED_LIBS)
        ufsrv_apply_build_options(${_PROJECT_NAME}_shared)
        ufsrv_build_summary(${_PROJECT_NAME}_shared)
    endif()
endif()

# ── Include directories ──────────────────────────────────────────────────────

# OBJECT target (build-tree consumers via add_subdirectory)
# The include roots the discovered dependencies reported. A path that was not
# found is left out rather than passed through: a NOTFOUND entry is a hard
# generate-time error, not an empty search.
set(_uflib_dependency_includes)
foreach(_dep_include IN ITEMS
        ${MariaDB_INCLUDE_DIRS} ${SQLite3_INCLUDE_DIRS} ${UTF8PROC_INCLUDE_DIRS} ${QRENCODE_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR} ${VALGRIND_INCLUDE_DIR})
    if(_dep_include)
        list(APPEND _uflib_dependency_includes "${_dep_include}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _uflib_dependency_includes)

target_include_directories(${_PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/uflib>
        $<INSTALL_INTERFACE:include>
        $<INSTALL_INTERFACE:include/uflib>
        ${_uflib_dependency_includes}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/adt
        ${CMAKE_CURRENT_SOURCE_DIR}/src/buffer_descriptor
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cdt
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db
        ${CMAKE_CURRENT_SOURCE_DIR}/src/exponential_backoff
        ${CMAKE_CURRENT_SOURCE_DIR}/src/logger
        ${CMAKE_CURRENT_SOURCE_DIR}/packages/K12
        ${CMAKE_CURRENT_SOURCE_DIR}/packages/lzf
        ${CMAKE_CURRENT_SOURCE_DIR}/packages/mjson
        ${CMAKE_CURRENT_SOURCE_DIR}/packages/protobuf-c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduled_jobs
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recycler
        ${CMAKE_CURRENT_SOURCE_DIR}/src/uuid4
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufconfig
        ${OPENSSL_INCLUDE_DIR}
)

# ── Propagate PUBLIC paths to installed targets ──────────────────────────────
set(_uflib_interface_includes
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/uflib>
)
foreach(_dep_include IN LISTS _uflib_dependency_includes)
    list(APPEND _uflib_interface_includes $<BUILD_INTERFACE:${_dep_include}>)
endforeach()
set(_uflib_interface_libs
    OpenSSL::SSL OpenSSL::Crypto
)
if(UFLIB_CAPABILITY_K12)
    list(APPEND _uflib_interface_libs uflib_k12)
endif()
if(UFLIB_CAPABILITY_MJSON)
    list(APPEND _uflib_interface_libs uflib_mjson)
endif()
if(UFLIB_CAPABILITY_LZF)
    list(APPEND _uflib_interface_libs uflib_lzf)
endif()
if(UFLIB_CAPABILITY_PROTOBUF_C)
    list(APPEND _uflib_interface_libs uflib_protobuf_c)
endif()
if(UFLIB_CAPABILITY_ZLOG)
    list(APPEND _uflib_interface_libs zlog_s)
endif()

if(UFLIB_CAPABILITY_QUIRC)
    list(APPEND _uflib_interface_libs uflib_quirc)
endif()
if(UFLIB_CAPABILITY_SQLITE)
    list(APPEND _uflib_interface_libs SQLite3::SQLite3)
endif()

if(IS_ROOT_PROJECT)
    target_include_directories(${_PROJECT_NAME}_static INTERFACE ${_uflib_interface_includes})
    target_link_libraries(${_PROJECT_NAME}_static INTERFACE ${_uflib_interface_libs})
    if(BUILD_SHARED_LIBS)
        target_include_directories(${_PROJECT_NAME}_shared INTERFACE ${_uflib_interface_includes})
        target_link_libraries(${_PROJECT_NAME}_shared INTERFACE ${_uflib_interface_libs})
    endif()
endif()

# ── Link dependencies (OBJECT target) ────────────────────────────────────────

if(NOT IS_ROOT_PROJECT AND UFSRV_LINKED_LIB_SSL STREQUAL "OPT" AND UFSRV_LINKED_LIB_CRYPTO STREQUAL "OPT")
    target_link_libraries(${_PROJECT_NAME} PUBLIC ssl crypto)
else()
    target_link_libraries(${_PROJECT_NAME} PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(UFLIB_CAPABILITY_SQLITE)
    target_link_libraries(${_PROJECT_NAME} PUBLIC SQLite3::SQLite3)
endif()

set(_uflib_packaged_libs)
if(UFLIB_CAPABILITY_K12)
    list(APPEND _uflib_packaged_libs uflib_k12)
endif()
if(UFLIB_CAPABILITY_MJSON)
    list(APPEND _uflib_packaged_libs uflib_mjson)
endif()
if(UFLIB_CAPABILITY_LZF)
    list(APPEND _uflib_packaged_libs uflib_lzf)
endif()
if(UFLIB_CAPABILITY_PROTOBUF_C)
    list(APPEND _uflib_packaged_libs uflib_protobuf_c)
endif()
if(UFLIB_CAPABILITY_ZLOG)
    list(APPEND _uflib_packaged_libs zlog_s)
endif()
# Guarded: target_link_libraries with an empty list is an error, and with every
# package switched off there is nothing to name.
if(_uflib_packaged_libs)
    target_link_libraries(${_PROJECT_NAME} PRIVATE ${_uflib_packaged_libs})
endif()

if(UFLIB_CAPABILITY_QUIRC)
    target_link_libraries(${_PROJECT_NAME} PRIVATE uflib_quirc)
endif()

# ── Position-independent code ─────────────────────────────────────────────────

set_target_properties(${_PROJECT_NAME} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME ${_PROJECT_NAME}
)

# for in-source consumers
set(${_PROJECT_NAME_X}_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include CACHE INTERNAL "")

# ── Sources — explicit listing (centralized per ufsrv convention) ────────────

# Third-party vendored packages (built as independent STATIC libraries)
if(UFLIB_CAPABILITY_K12)
    add_subdirectory(packages/K12)
endif()
if(UFLIB_CAPABILITY_LZF)
    add_subdirectory(packages/lzf)
endif()
if(UFLIB_CAPABILITY_MJSON)
    add_subdirectory(packages/mjson)
endif()
if(UFLIB_CAPABILITY_PROTOBUF_C)
    add_subdirectory(packages/protobuf-c)
endif()

# quirc is called into by src/qr_code/ only, which is itself gated on the same
# switch, so a build that does not ask for it should not pay for it.
if(UFLIB_CAPABILITY_QUIRC)
    set(UFLIB_QUIRC_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/uflib/quirc)
    add_subdirectory(packages/quirc)
    message(STATUS "QR-code module: reading capability — libuflib_quirc.a built and folded in")
endif()

if(UFLIB_CAPABILITY_QRENCODE)
    target_include_directories(${_PROJECT_NAME} PRIVATE ${QRENCODE_INCLUDE_DIRS})
endif()
if(UFLIB_CAPABILITY_UTF8PROC)
    target_include_directories(${_PROJECT_NAME} PRIVATE ${UTF8PROC_INCLUDE_DIRS})
endif()

if(UFLIB_CAPABILITY_ZLOG)
    add_subdirectory(packages/zlog)
endif()

if(UFLIB_CAPABILITY_ZLOG)
    set_target_properties(zlog_s PROPERTIES
        EXPORT_NAME zlog
        POSITION_INDEPENDENT_CODE ON)
endif()

# ── Fold the vendored archives into uflib's own archive ─────────────────────
#
if(IS_ROOT_PROJECT)
    set(_UFLIB_MERGE_DIR    ${CMAKE_CURRENT_BINARY_DIR}/.merged)
    set(_UFLIB_MERGE_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/uflib_merge_archives.mri)

    set(_UFLIB_MERGE_ADDLIBS "addlib $<TARGET_FILE:${_PROJECT_NAME}_static>\n")
    set(_UFLIB_MERGE_DEPS)

    if(UFLIB_CAPABILITY_K12)
        string(APPEND _UFLIB_MERGE_ADDLIBS "addlib $<TARGET_FILE:uflib_k12>\n")
        list(APPEND _UFLIB_MERGE_DEPS uflib_k12)
    endif()
    if(UFLIB_CAPABILITY_LZF)
        string(APPEND _UFLIB_MERGE_ADDLIBS "addlib $<TARGET_FILE:uflib_lzf>\n")
        list(APPEND _UFLIB_MERGE_DEPS uflib_lzf)
    endif()
    if(UFLIB_CAPABILITY_MJSON)
        string(APPEND _UFLIB_MERGE_ADDLIBS "addlib $<TARGET_FILE:uflib_mjson>\n")
        list(APPEND _UFLIB_MERGE_DEPS uflib_mjson)
    endif()
    if(UFLIB_CAPABILITY_PROTOBUF_C)
        string(APPEND _UFLIB_MERGE_ADDLIBS "addlib $<TARGET_FILE:uflib_protobuf_c>\n")
        list(APPEND _UFLIB_MERGE_DEPS uflib_protobuf_c)
    endif()
    if(UFLIB_CAPABILITY_ZLOG)
        string(APPEND _UFLIB_MERGE_ADDLIBS "addlib $<TARGET_FILE:zlog_s>\n")
        list(APPEND _UFLIB_MERGE_DEPS zlog_s)
    endif()

    if(UFLIB_CAPABILITY_QUIRC)
        string(APPEND _UFLIB_MERGE_ADDLIBS "addlib $<TARGET_FILE:uflib_quirc>\n")
        list(APPEND _UFLIB_MERGE_DEPS uflib_quirc)
    endif()

    file(GENERATE OUTPUT ${_UFLIB_MERGE_SCRIPT} CONTENT
"create ${_UFLIB_MERGE_DIR}/libuflib.a
${_UFLIB_MERGE_ADDLIBS}save
end
")

    add_custom_command(TARGET ${_PROJECT_NAME}_static POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_UFLIB_MERGE_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_UFLIB_MERGE_DIR}
        COMMAND sh -c "${CMAKE_AR} -M < ${_UFLIB_MERGE_SCRIPT}"
        COMMAND ${CMAKE_COMMAND} -E copy ${_UFLIB_MERGE_DIR}/libuflib.a
                $<TARGET_FILE:${_PROJECT_NAME}_static>
        COMMENT "Folding the vendored archives into libuflib.a"
        VERBATIM)

    add_dependencies(${_PROJECT_NAME}_static ${_UFLIB_MERGE_DEPS})
endif()

# ── Link closure for the shared library ──────────────────────────────────────
# Both archives are built from $<TARGET_OBJECTS:...>, which carries no usage
# requirements, so nothing named above reaches them. The static archive is
# repaired by the merge above; the shared one has to say the same thing again:
# the folded set goes in whole (libuflib.a hands those APIs to its consumers, so
# libuflib.so has to as well), and everything the code calls out to becomes a
# DT_NEEDED entry. Without this the .so records no dependency beyond libc and
# leaves every mysql_*, redis*, zlog_*, quirc_*, mjson_*, QRcode_* and
# utf8proc_* call for the consumer to resolve by hand — which is how "-luflib
# alone does not close the link" presented itself.
#
# Derived from the same capability flags as everything else, so a minimal
# configuration gains no entry it does not use. Note that UFLIB_SHARED_EXPORT_ALL
# currently exports the embedded vendored symbols too; that ends when the export
# sweep turns the interim switch off.
if(BUILD_SHARED_LIBS AND IS_ROOT_PROJECT)
    set(_uflib_shared_link_libs)
    foreach(_vendored IN LISTS _UFLIB_MERGE_DEPS)
        list(APPEND _uflib_shared_link_libs "$<LINK_LIBRARY:WHOLE_ARCHIVE,${_vendored}>")
    endforeach()

    if(UFLIB_CAPABILITY_HIREDIS)
        list(APPEND _uflib_shared_link_libs uflib_hiredis)
    endif()
    if(UFLIB_CAPABILITY_SQL)
        list(APPEND _uflib_shared_link_libs ${MariaDB_LIBRARIES})
    endif()
    if(UFLIB_CAPABILITY_QRENCODE)
        list(APPEND _uflib_shared_link_libs ${QRENCODE_LIBRARIES})
    endif()
    if(UFLIB_CAPABILITY_UTF8PROC)
        list(APPEND _uflib_shared_link_libs ${UTF8PROC_LIBRARIES})
    endif()
    if(UFLIB_CAPABILITY_SQLITE)
        list(APPEND _uflib_shared_link_libs SQLite3::SQLite3)
    endif()

    if(TARGET OpenSSL::SSL)
        list(APPEND _uflib_shared_link_libs OpenSSL::SSL OpenSSL::Crypto)
    else()
        list(APPEND _uflib_shared_link_libs ssl crypto)
    endif()

    # floor/ceilf/ldexp: glibc does not export libm's names, and the .pc names
    # -lm for static consumers for the same reason.
    list(APPEND _uflib_shared_link_libs m)

    target_link_libraries(${_PROJECT_NAME}_shared PRIVATE ${_uflib_shared_link_libs})
endif()

# Native uflib sources — all compiled into the single uflib OBJECT target
target_sources(${_PROJECT_NAME} PRIVATE
    # ── Core (src/) ──────────────────────────────────────────────────────
    src/tokeniser.c
    src/ufsrvuid.c
    src/utils.c
    src/utils_base32.c
    src/utils_base64.c
    src/utils_base64url.c
    src/utils_bits.c
    src/utils_collection_str.c
    src/utils_crypto.c
    src/utils_file.c
    src/utils_file_loader.c
    src/utils_hex.c
    src/utils_iec_quantity.c
    src/utils_json.c
    src/utils_random.c
    src/utils_secrets.c
    src/utils_str.c
    src/utils_sys.c
    src/utils_time.c
    src/utils_threads.c
    src/utils_url.c

    # ── Resource router (src/resource_router/) ───────────────────────────
    src/resource_router/resource_router.c

    # ── ADT (src/adt/) ───────────────────────────────────────────────────
    src/adt/adt_doubly_linkedlist.c
    src/adt/adt_hopscotch_hashtable.c
    src/adt/adt_hashtable.c
    src/adt/adt_distinct_array.c
    src/adt/adt_linkedlist.c
    src/adt/adt_locking_lru.c
    src/adt/adt_minheap.c
    src/adt/adt_queue.c

    # ── ADT / hashtable_v2 ───────────────────────────────────────────────
    src/adt/hashtable_v2/hashtable_v2.c

    src/adt/hopscotch_hashtable_v2/hopscotch_hashtable_v2.c

    # ── buffer_descriptor  ────────────────────────────────────────────────────
    src/buffer_descriptor/buffer_descriptor.c

    # ── CDT / alias_map ──────────────────────────────────────────────────
    src/cdt/alias_map/alias_map.c
    src/cdt/alias_map/alias_map_json.c
    src/cdt/alias_map/alias_map_snapshot.c

    # ── CDT / chase_lev_stealing_queue ───────────────────────────────────
    src/cdt/chase_lev_stealing_queue/cdt_chase_lev_stealing_queue.c

    # ── CDT / hashmap ────────────────────────────────────────────────────
    src/cdt/hashmap/cdt_fixed_width_hashmap.c

    # ── CDT / lamport_queue ──────────────────────────────────────────────
    src/cdt/lamport_queue/cdt_lamport_queue.c

    # ── CDT / lockless_lru ───────────────────────────────────────────────
    src/cdt/lockless_lru/cdt_lockless_lru.c

    # ── CDT / mpsc_queue ─────────────────────────────────────────────────
    src/cdt/mpsc_queue/cdt_mpsc_queue.c

    # ── CDT / lockless_ringbuffer ────────────────────────────────────────
    src/cdt/lockless_ringbuffer/cdt_lockless_ringbuffer.c

    # ── CDT / lockless_treiber_stack ─────────────────────────────────────
    src/cdt/lockless_treiber_stack/lockless_treiber_stack.c

    # ── CDT / lockless_minheap ───────────────────────────────────────────
    src/cdt/lockless_minheap/cdt_lockless_minheap.c

    # ── DB (src/db/) ─────────────────────────────────────────────────────
    $<$<BOOL:${_uflib_db_enabled}>:${CMAKE_CURRENT_SOURCE_DIR}/src/db/db_abstraction.c>
    $<$<BOOL:${UFLIB_CAPABILITY_SQL}>:${CMAKE_CURRENT_SOURCE_DIR}/src/db/db_mysql.c>
    $<$<BOOL:${UFLIB_CAPABILITY_SQLITE}>:${CMAKE_CURRENT_SOURCE_DIR}/src/db/db_sqlite.c>
    $<$<BOOL:${_uflib_db_enabled}>:${CMAKE_CURRENT_SOURCE_DIR}/src/db/db_ops.c>

    # ── Net (src/net/) ───────────────────────────────────────────────────
    src/net/utils_net.c
    src/net/network_socket_address.c
    src/net/RFC4193.c

    # ── Recycler (src/recycler/) ─────────────────────────────────────────
    src/recycler/instances_list.c
    src/recycler/recycler.c

    # ── RecyclerV2 (src/recycler_v2/) ────────────────────────────────────
    src/recycler_v2/recycler_v2.c
    src/recycler_v2/recycler_v2_storage.c

    # ── Scheduled Jobs (src/scheduled_jobs/) ─────────────────────────────
    src/scheduled_jobs/scheduled_jobs.c

    # ── Thread Worker (src/thread_worker/) ───────────────────────────────
    src/thread_worker/file_loader_concurrent_access/file_loader_concurrent_access.c

    # ── UUID v4 (src/uuid4/) ─────────────────────────────────────────────
    src/uuid4/uuid.c

    # ── Exponential Backoff (src/exponential_backoff/) ───────────────────
    src/exponential_backoff/algorithm.c

    # ── Logger (src/logger/) ─────────────────────────────────────────────
    # The public interface, the configuration compiler, and the binding point for whichever backend this build carries..
    src/logger/logger.c

    # ── mjson extension (src/mjson/) ─────────────────────────────────────
    # uflib's wildcard / recursive-descent wrapper over the vendored parser, so it goes with the parser it wraps.
    $<$<BOOL:${UFLIB_CAPABILITY_MJSON}>:${CMAKE_CURRENT_SOURCE_DIR}/src/mjson/mjson_ex.c>

    # ── Logger backend — zlog (src/logger/driver/zlog/) ──────────────────
    $<$<BOOL:${UFLIB_CAPABILITY_ZLOG}>:${CMAKE_CURRENT_SOURCE_DIR}/src/logger/driver/zlog/logger_zlog.c>

    # ── Config module (src/ufconfig/) ─────────────────────────────────────
    src/ufconfig/ufconfig.c
    src/ufconfig/ufconfig_parse.c
    src/ufconfig/ufconfig_canon.c
    src/ufconfig/ufconfig_pairs.c
    src/ufconfig/ufconfig_transform.c
    src/ufconfig/ufconfig_validate.c
    src/ufconfig/ufconfig_namespace.c
    src/ufconfig/ufconfig_util.c

    # ── Config module — driver layer (src/ufconfig/driver/) ───────────────
    src/ufconfig/driver/ufconfig_driver.c
    src/ufconfig/driver/ufconfig_driver_file.c
    src/ufconfig/driver/ufconfig_driver_mem.c
    $<$<BOOL:${UFLIB_CAPABILITY_SQL}>:${CMAKE_CURRENT_SOURCE_DIR}/src/ufconfig/driver/ufconfig_driver_sql.c>
    $<$<BOOL:${UFLIB_CAPABILITY_HIREDIS}>:${CMAKE_CURRENT_SOURCE_DIR}/src/ufconfig/driver/ufconfig_driver_redis.c>

    # ── QR-code module (src/qr_code/) ─────────────────────────────────────
    $<$<BOOL:${UFLIB_CAPABILITY_QUIRC}>:${CMAKE_CURRENT_SOURCE_DIR}/src/qr_code/utils_qr_code.c>
    $<$<BOOL:${UFLIB_CAPABILITY_QRENCODE}>:${CMAKE_CURRENT_SOURCE_DIR}/src/qr_code/utils_qr_code_encode.c>

    # ── Command module (src/ufcommand/) ────────────────────────────────────
    src/ufcommand/ufcommand.c
    src/ufcommand/ufcommand_priv.c
    src/ufcommand/ufcommand_persistence.c
    src/ufcommand/ufcommand_telemetry.c
    src/ufcommand/ufcommand_completion.c

    # ── Optional (src/optional/) ──────────────────────────────────────────
    # An owning container for one payload or none; no dependencies past libc,
    # so it is unconditional like every other module that carries none.
    src/optional/ufoptional.c

    # ── Message framing: WebSocket (src/msg_framing/websocket/) ───────────
    # RFC 6455 frame codec. Self-contained: no logger, socket or allocation.
    src/msg_framing/websocket/ws_frame.c
    src/msg_framing/websocket/ws_message.c
    src/msg_framing/websocket/ws_utf8.c
    src/msg_framing/websocket/ws_close.c
    src/msg_framing/websocket/ws_handshake.c

)

# ── Config module — Redis driver (opt-in) ────────────────────────────────────
if(UFLIB_CAPABILITY_HIREDIS)
    add_subdirectory(packages/hiredis)
    target_include_directories(${_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/packages/hiredis)
    message(STATUS "Config module: Redis driver enabled — link hiredis in the consumer")
endif()

# ── Config module — SQL driver (capability) ──────────────────────────────────
if(UFLIB_CAPABILITY_SQL)
    message(STATUS "Config module: SQL driver enabled — link libmariadb in the consumer")
endif()

if(UFLIB_CAPABILITY_SQLITE)
    message(STATUS "Config module: SQLite driver enabled — SQLite3::SQLite3 on the link interface")
endif()

# ── Dynamic version — generated into build directory ─────────────────────────

add_custom_target(${_PROJECT_NAME}_version
    COMMAND chmod 755 ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/version.c
    COMMENT "Generating version.c with a fresh compilation timestamp"
    VERBATIM
)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/version.c PROPERTIES GENERATED TRUE)
target_sources(${_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.c)
add_dependencies(${_PROJECT_NAME} ${_PROJECT_NAME}_version)

# ── Install ──────────────────────────────────────────────────────────────────

if(_PACKAGE_INSTALL)
    include(GNUInstallDirs)

    install(TARGETS ${_PROJECT_NAME}_static
        EXPORT ${_PROJECT_NAME}-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    if(UFLIB_CAPABILITY_K12)
        install(TARGETS uflib_k12
            EXPORT ${_PROJECT_NAME}-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()
    if(UFLIB_CAPABILITY_MJSON)
        install(TARGETS uflib_mjson
            EXPORT ${_PROJECT_NAME}-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()
    if(UFLIB_CAPABILITY_LZF)
        install(TARGETS uflib_lzf
            EXPORT ${_PROJECT_NAME}-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()
    if(UFLIB_CAPABILITY_PROTOBUF_C)
        install(TARGETS uflib_protobuf_c
            EXPORT ${_PROJECT_NAME}-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()
    if(UFLIB_CAPABILITY_ZLOG)
        install(TARGETS zlog_s
            EXPORT ${_PROJECT_NAME}-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()

    if(UFLIB_CAPABILITY_QUIRC)
        install(TARGETS uflib_quirc
            EXPORT ${_PROJECT_NAME}-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()

    if(BUILD_SHARED_LIBS)
        install(TARGETS ${_PROJECT_NAME}_shared
            EXPORT ${_PROJECT_NAME}-targets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "K12" EXCLUDE
        PATTERN "lzf" EXCLUDE
        PATTERN "mjson" EXCLUDE
        PATTERN "protobuf-c" EXCLUDE
        PATTERN "zlog" EXCLUDE
    )
    # include/uflib is already an INSTALL_INTERFACE root, so the bare
    # <config_uflib.h> the library itself uses resolves for consumers too.
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/config_uflib.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uflib
    )
    if(UFLIB_CAPABILITY_K12)
        install(DIRECTORY include/uflib/K12 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uflib)
    endif()
    if(UFLIB_CAPABILITY_LZF)
        install(DIRECTORY include/uflib/lzf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uflib)
    endif()
    if(UFLIB_CAPABILITY_MJSON)
        install(DIRECTORY include/uflib/mjson DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uflib)
    endif()
    if(UFLIB_CAPABILITY_PROTOBUF_C)
        install(DIRECTORY include/uflib/protobuf-c DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uflib)
    endif()
    if(UFLIB_CAPABILITY_ZLOG)
        install(DIRECTORY include/uflib/zlog DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uflib)
    endif()

    # man page
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/docs/man/${_PROJECT_NAME}.7
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man7
    )

    # pkg-config
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_PROJECT_NAME}.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}.pc
        @ONLY
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

    # cmake package config
    include(CMakePackageConfigHelpers)
    set(INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/${_PROJECT_NAME})

    install(EXPORT ${_PROJECT_NAME}-targets
        FILE ${_PROJECT_NAME}Targets.cmake
        NAMESPACE ${_PROJECT_NAME}::
        DESTINATION ${INSTALL_CONFIGDIR}
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_PROJECT_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
    )

    # Register package in the User Package Registry
    set(CMAKE_EXPORT_PACKAGE_REGISTRY ON)
    export(PACKAGE ${_PROJECT_NAME})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}ConfigVersion.cmake
        VERSION ${${_PROJECT_NAME_X}_VERSION}
        COMPATIBILITY AnyNewerVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}ConfigVersion.cmake
        DESTINATION ${INSTALL_CONFIGDIR}
    )

    # ── IDE source manifest ──────────────────────────────────────────────────
    # The installed package ships headers and a binary, never sources, so a
    # consumer that links it has no definition to navigate to. Publish the
    # translation units this library actually compiles, together with the
    # checkout they were configured from, so a consumer can index them.
    #
    # Paths are absolute and machine-specific by design: the manifest records
    # where the sources were when the package was built. A consumer must
    # tolerate entries that do not exist on its machine and fall back to
    # whatever it can resolve itself (other checkout location, another host).
    get_target_property(_ufsrv_ide_sources ${_PROJECT_NAME} SOURCES)
    set(_ufsrv_ide_manifest
        "set(${_PROJECT_NAME_X}_IDE_SOURCE_DIR \"${CMAKE_CURRENT_SOURCE_DIR}\")\nset(${_PROJECT_NAME_X}_IDE_SOURCES\n")
    foreach(_ufsrv_ide_src IN LISTS _ufsrv_ide_sources)
        if(NOT IS_ABSOLUTE "${_ufsrv_ide_src}")
            set(_ufsrv_ide_src "${CMAKE_CURRENT_SOURCE_DIR}/${_ufsrv_ide_src}")
        endif()
        # Generated sources live in the build tree, not the source checkout:
        # there is no file for a consumer to navigate to.
        string(FIND "${_ufsrv_ide_src}" "${CMAKE_CURRENT_BINARY_DIR}/" _ufsrv_ide_gen)
        if(_ufsrv_ide_gen EQUAL 0)
            continue()
        endif()
        string(APPEND _ufsrv_ide_manifest "    \"${_ufsrv_ide_src}\"\n")
    endforeach()
    string(APPEND _ufsrv_ide_manifest ")\n")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}-ide-sources.cmake"
        "${_ufsrv_ide_manifest}")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}-ide-sources.cmake
        DESTINATION ${INSTALL_CONFIGDIR}
    )

    # archive-uninstall: move currently installed files to /tmp before installing new version
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/archive_uninstall.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/archive_uninstall.cmake
        @ONLY
    )
    add_custom_target(archive-uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/archive_uninstall.cmake
        COMMENT "Archiving currently installed ${_PROJECT_NAME} to /tmp/archived_${_PROJECT_NAME}_${${_PROJECT_NAME_X}_VERSION}"
        VERBATIM
    )

    # Uninstall target
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
        @ONLY
    )
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    )

    # Install verification — informational only (never fails the build).
    # Compares the uflib version across system install, git HEAD, and local copy.
    add_custom_target(install_verification
        COMMAND ${CMAKE_COMMAND}
            -DINSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/install_verification.cmake
        COMMENT "Verifying uflib version across system install, git, and local copy"
        VERBATIM
    )

    # CPack (Debian packaging)
    set(CPACK_PACKAGE_NAME "${_PROJECT_NAME}-dev")
    set(CPACK_PACKAGE_VERSION "${${_PROJECT_NAME_X}_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C utility library for ufsrv (ADTs, crypto, DB abstraction, networking)")
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "unfacd works")
    set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
    # libssl-dev, not libssl3: uflib.pc and uflibConfig.cmake both need OpenSSL's development files.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libssl-dev")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    include(CPack)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────

if(_PACKAGE_TESTS)
    add_subdirectory(tests)
endif()
