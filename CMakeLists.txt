cmake_minimum_required(VERSION 3.20)

set(_PROJECT_NAME "uflib")
string(TOUPPER ${_PROJECT_NAME} _PROJECT_NAME_X)

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
SET(UFLIB_VERSION "${UFLIB_MAJOR}.${UFLIB_MINOR}.${UFLIB_PATCH}")
MESSAGE("Detected ${_PROJECT_NAME} version: ${UFLIB_VERSION}")
#end extract current lib version

project(${_PROJECT_NAME} LANGUAGES C ASM)
project(${_PROJECT_NAME} VERSION "${UFLIB_VERSION}")

set(CMAKE_C_STANDARD 11)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IS_ROOT_PROJECT ON)
else()
    set(IS_ROOT_PROJECT OFF)
endif()

option(_PACKAGE_TESTS "Enable to generate test targets" ${IS_ROOT_PROJECT})
option(_PACKAGE_INSTALL "Enable to add install target" ${IS_ROOT_PROJECT})

# Offer the user the choice of overriding the installation directories
set(INSTALL_LIB_DIR lib CACHE PATH "Installation directory for libraries")
set(INSTALL_BIN_DIR bin CACHE PATH "Installation directory for executables")
set(INSTALL_INCLUDE_DIR include CACHE PATH
        "Installation directory for header files")
if(WIN32 AND NOT CYGWIN)
    set(DEF_INSTALL_CMAKE_DIR CMake)
else()
    set(DEF_INSTALL_CMAKE_DIR lib/CMake/${_PROJECT_NAME})
endif()
set(INSTALL_CMAKE_DIR ${DEF_INSTALL_CMAKE_DIR} CACHE PATH
        "Installation directory for CMake files")

include(cmake/CPM.cmake)#for managing package dependencies

add_library(${_PROJECT_NAME} "")
add_subdirectory(src)
add_subdirectory(src/adt)
add_subdirectory(src/db)
add_subdirectory(src/K12)
add_subdirectory(src/lzf)
add_subdirectory(src/protobuf-c)
add_subdirectory(src/scheduled_jobs)

#force generation of fresh build time
add_custom_target (
        ${_PROJECT_NAME}_version
        COMMAND chmod 755 ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_dependencies(${_PROJECT_NAME} ${_PROJECT_NAME}_version)
#end force

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

include(GNUInstallDirs)
set(INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/${_PROJECT_NAME})

#https://pabloariasal.github.io/2018/02/19/its-time-to-do-cmake-right/
#Export uflib's configuration by adding targets to an export group:
install(TARGETS ${_PROJECT_NAME}
        EXPORT ${_PROJECT_NAME}-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )

#In case a different name is desired for the export target (last arg)
#set_target_properties(${_PROJECT_NAME} PROPERTIES EXPORT_NAME ${_PROJECT_NAME})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

#Export the targets to a script
install(EXPORT ${_PROJECT_NAME}-targets
        FILE
        ${_PROJECT_NAME}Targets.cmake
        NAMESPACE
        ${_PROJECT_NAME}::
        DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/${_PROJECT_NAME}
        )

#This package allows us to write config files that can be relocated (eg in-source); ie where paths are not hard-coded.
include(CMakePackageConfigHelpers)

#configure our template
configure_package_config_file(
        ${CMAKE_CURRENT_LIST_DIR}/cmake/${_PROJECT_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
        )

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

## Exporting from the build tree
export(EXPORT ${_PROJECT_NAME}-targets
        FILE ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Targets.cmake
        NAMESPACE ${_PROJECT_NAME}::)

#Register package in the User Package Registry
export(PACKAGE ${_PROJECT_NAME})

