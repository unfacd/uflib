#invoke from project root with 1) cmake -B cmake-build-debug -G "unix" -S .
#                              2) cmake --build cmake-build-debug -D CMAKE_BUILD_TYPE=Debug
cmake_minimum_required(VERSION 3.20)

set(_PROJECT_NAME "uflib")
string(TOUPPER ${_PROJECT_NAME} _PROJECT_NAME_X)

include(CMakePrintHelpers)#cmake_print_variables
include(CMakeDependentOption)#enable cmake_dependent_option()
include(CheckIncludeFile)
include (CheckSymbolExists)
include(CheckFunctionExists)

#extract current lib version
MACRO(getVersionPatchLevel name)
    SET(VERSION_REGEX "^#define ${name} (.+)$")
    FILE(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h"
            VERSION_BIT REGEX ${VERSION_REGEX})
    STRING(REGEX REPLACE ${VERSION_REGEX} "\\1" ${name} "${VERSION_BIT}")
ENDMACRO(getVersionPatchLevel)
#end extract version

getVersionPatchLevel(UFLIB_MAJOR)
getVersionPatchLevel(UFLIB_MINOR)
getVersionPatchLevel(UFLIB_PATCH)
set(UFLIB_VERSION "${UFLIB_MAJOR}.${UFLIB_MINOR}.${UFLIB_PATCH}")
message("Detected ${_PROJECT_NAME} version: ${UFLIB_VERSION}")

project(${_PROJECT_NAME} LANGUAGES C ASM)
project(${_PROJECT_NAME} VERSION "${UFLIB_VERSION}")

set(CMAKE_C_STANDARD 11)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IS_ROOT_PROJECT ON)
else()
    set(IS_ROOT_PROJECT OFF)
endif()

cmake_print_variables(CMAKE_BUILD_TYPE)

option(_PACKAGE_TESTS "Enable to generate test targets" ${IS_ROOT_PROJECT})
option(_PACKAGE_INSTALL "Enable to add install target" ${IS_ROOT_PROJECT})
option(${_PROJECT_NAME_X}_ENABLE_VALGRIND "Turn this ON to enable valgrind instrumentation" OFF)
option(${_PROJECT_NAME_X}_ENABLE_SANITIZE "Turn this ON to enable GCC sanitizer" ON)
option(${_PROJECT_NAME_X}_USE_ASSERT "Enable assertions" ON)
option({_PROJECT_NAME_X}_BUILD_DOCS "Build documentation" OFF)

cmake_print_variables(${_PROJECT_NAME_X}_ENABLE_SANITIZE ${_PROJECT_NAME_X}_ENABLE_VALGRIND)
add_custom_target(_print_valgrid_state COMMAND ${CMAKE_COMMAND} -E echo "$<BOOL:${_PROJECT_NAME_X}_ENABLE_VALGRIND>") #use with command 'make _print_valgrid_state'

#valgrind cannot be on at the same time sanitizer is
if("${_PROJECT_NAME_X}_ENABLE_SANITIZE")
    set(${_PROJECT_NAME_X}_ENABLE_VALGRIND OFF)
elseif(${_PROJECT_NAME_X}_ENABLE_VALGRIND)
    set(${_PROJECT_NAME_X}_ENABLE_SANITIZE OFF)
endif()

if(${_PROJECT_NAME_X}_ENABLE_VALGRIND)
    check_include_file(valgrind/valgrind.h HAVE_VALGRIND_VALGRIND_H)
endif()

list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(alloca "alloca.h;stdlib.h;cstdlib" _HAVE_ALLOCA)
#list(APPEND CMAKE_REQUIRED_LIBRARIES xxx)
list(REMOVE_ITEM CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)

# Offer the user the choice of overriding the installation directories
set(INSTALL_LIB_DIR lib CACHE PATH "Installation directory for libraries")
set(INSTALL_BIN_DIR bin CACHE PATH "Installation directory for executables")
set(INSTALL_INCLUDE_DIR include CACHE PATH "Installation directory for header files")
if(WIN32 AND NOT CYGWIN)
    set(DEF_INSTALL_CMAKE_DIR CMake)
else()
    set(DEF_INSTALL_CMAKE_DIR lib/CMake/${_PROJECT_NAME})
endif()
set(INSTALL_CMAKE_DIR ${DEF_INSTALL_CMAKE_DIR} CACHE PATH "Installation directory for CMake files")

include(cmake/CPM.cmake)#for managing package dependencies

add_library(${_PROJECT_NAME} "")
add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME})

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/uflib_config.h.cmake.in ${CMAKE_CURRENT_SOURCE_DIR}/src/config.h)

add_subdirectory(src)
add_subdirectory(src/adt)
add_subdirectory(src/db)
add_subdirectory(src/K12)
add_subdirectory(src/lzf)
add_subdirectory(src/protobuf-c)
add_subdirectory(src/scheduled_jobs)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    if(${_PROJECT_NAME_X}_ENABLE_SANITIZE)
        set(${PROJECT_NAME_X}_COMPILE_FLAGS_RELEASE "-O2 -Wall -Wno-unused-label -Wno-unused-variable -Wno-comment -fsanitize=address -fsanitize=nonnull-attribute -fsanitize=bool")
        set(${PROJECT_NAME_X}_COMPILE_FLAGS_DEBUG "-O0 -ggdb3 -Wall -Wno-unused-label -Wno-unused-variable -Wno-comment -fuse-ld=gold fsanitize=address -fsanitize=nonnull-attribute -fsanitize=bool")
    endif()
    if(${_PROJECT_NAME_X}_ENABLE_VALGRIND)
        set(${PROJECT_NAME_X}_COMPILE_FLAGS_RELEASE "-O2 -Wall -Wno-unused-label -Wno-unused-variable -Wno-comment")
        set(${PROJECT_NAME_X}_COMPILE_FLAGS_DEBUG "-O0 -ggdb3 -Wall -Wno-unused-label -Wno-unused-variable -Wno-comment -fuse-ld=gold")
    endif()

    target_compile_definitions(${_PROJECT_NAME}
            PUBLIC
            $<$<CONFIG:Debug>:${PROJECT_NAME_X}_COMPILE_FLAGS_DEBUG>
            $<$<CONFIG:Release>:${PROJECT_NAME_X}_COMPILE_FLAGS_RELEASE>
            )
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 11)
        #...
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
endif()

cmake_print_variables(${_PROJECT_NAME_X}_ENABLE_SANITIZE ${_PROJECT_NAME_X}_ENABLE_VALGRIND)

#force generation of fresh build time
add_custom_target (
        ${_PROJECT_NAME}_version
        COMMAND chmod 755 ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_dependencies(${_PROJECT_NAME} ${_PROJECT_NAME}_version)
#end force

include(CheckSymbolExists)
if (NOT _GNU_SOURCE)
    check_symbol_exists(__GNU_LIBRARY__ "features.h" _GNU_SOURCE)
    if (NOT _GNU_SOURCE)
        unset(_GNU_SOURCE CACHE)
        check_symbol_exists(_GNU_SOURCE "features.h" _GNU_SOURCE)
    endif ()
endif ()

if (_GNU_SOURCE)
    target_compile_definitions(${_PROJECT_NAME} PUBLIC GNU_SOURCE=1)
else()
    message(FATAL_ERROR "${_PROJECT_NAME} requires _GNU_SOURCE")
endif ()

if(_PACKAGE_TESTS)
    CPMAddPackage(
            NAME googletest
            GITHUB_REPOSITORY google/googletest
            GIT_TAG release-1.11.0
            VERSION 1.11.0
            OPTIONS "INSTALL_GTEST OFF" "gtest_force_shared_crt"
    )
    enable_testing()
    add_subdirectory(test)
endif()

find_package(OpenSSL REQUIRED)
if (OPENSSL_FOUND)
    target_include_directories(${_PROJECT_NAME} PUBLIC ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(${_PROJECT_NAME} OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "Found OpenSSL ${OPENSSL_VERSION}")
else()
    message(STATUS "OpenSSL Not Found")
endif()

target_include_directories(${_PROJECT_NAME}
        PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        "/opt/include"
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

#this allows users of in-source lib to  directly do target_include_directories (xxtarget PUBLIC ${UFLIB_INCLUDE_DIR})
set(${_PROJECT_NAME_X}_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include CACHE INTERNAL "")

set_target_properties(${_PROJECT_NAME}
                        PROPERTIES
                        POSITION_INDEPENDENT_CODE 1
                        DEBUG_POSTFIX "_d"
                    )
include(GNUInstallDirs)
set(INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/${_PROJECT_NAME})

#install build targets (only lib for this package):
install(TARGETS ${_PROJECT_NAME}
        EXPORT ${_PROJECT_NAME}-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT lib
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT lib
        )

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

include(CMakePackageConfigHelpers)

##support for in-source lib linking
set (${_PROJECT_NAME_X}_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include" )
message(">>> INCLUDE_DIRS for in-source lib: ${${_PROJECT_NAME}_INCLUDE_DIRS}... (CMAKE_CURRENT_LIST_DIR: ${CMAKE_CURRENT_LIST_DIR})")

configure_package_config_file(
        ${CMAKE_CURRENT_LIST_DIR}/cmake/${_PROJECT_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
        PATH_VARS ${_PROJECT_NAME_X}_INCLUDE_DIRS
)
#Exporting from the build tree
export(EXPORT ${_PROJECT_NAME}-targets
        FILE ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Targets.cmake
        NAMESPACE ${_PROJECT_NAME}::)

#Register package in the User Package Registry
set(CMAKE_EXPORT_PACKAGE_REGISTRY ON)
export(PACKAGE ${_PROJECT_NAME})
#############

#configure our template for standard system installed lib configuration
set (${_PROJECT_NAME_X}_INCLUDE_DIRS "include" )
#message(">>> INCLUDE_DIRS for installed lib: ${${_PROJECT_NAME}_INCLUDE_DIRS}...")

configure_package_config_file(
        ${CMAKE_CURRENT_LIST_DIR}/cmake/${_PROJECT_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
        PATH_VARS ${_PROJECT_NAME_X}_INCLUDE_DIRS
)
install(EXPORT ${_PROJECT_NAME}-targets
        FILE
        ${_PROJECT_NAME}Targets.cmake
        NAMESPACE
        ${_PROJECT_NAME}::
        DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/${_PROJECT_NAME}
        )
#######################

#Create a ConfigVersion.cmake file
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}ConfigVersion.cmake
        VERSION ${UFLIB_VERSION}
        COMPATIBILITY AnyNewerVersion
)

#Install the Config, Configversion and other custom modules
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}ConfigVersion.cmake
        DESTINATION ${INSTALL_CONFIGDIR}
        )

if({_PROJECT_NAME_X}_BUILD_DOCS)
    include(cmake/doxygen.cmake)
endif()