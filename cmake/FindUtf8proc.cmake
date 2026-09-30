# FindUtf8proc.cmake — locate libutf8proc.
#
# A find-module because upstream ships no CMake package, only libutf8proc.pc —
# the FindQREncode.cmake arrangement.
#
#   Utf8proc_FOUND         header and library both found
#   UTF8PROC_INCLUDE_DIRS  for target_include_directories
#   UTF8PROC_LIBRARIES     for target_link_libraries
#   UTF8PROC_VERSION       from the pkg-config file, when it is there
#
# /opt first, then the default paths: this fleet installs it under /opt, but a
# host that installs it system-wide should still be found.

include(FindPackageHandleStandardArgs)

find_path(UTF8PROC_INCLUDE_DIR
    NAMES utf8proc.h
    PATHS /opt/include
)

find_library(UTF8PROC_LIBRARY
    NAMES utf8proc
    PATHS /opt/lib
)

if(UTF8PROC_LIBRARY AND NOT UTF8PROC_VERSION)
    # From the library's own directory: the .pc sits beside what it describes.
    get_filename_component(_utf8proc_libdir "${UTF8PROC_LIBRARY}" DIRECTORY)
    set(UTF8PROC_PKGCONFIG_FILE "${_utf8proc_libdir}/pkgconfig/libutf8proc.pc")

    if(EXISTS "${UTF8PROC_PKGCONFIG_FILE}")
        file(STRINGS "${UTF8PROC_PKGCONFIG_FILE}" _utf8proc_pc_version
            REGEX "^Version:[ \t]*[0-9]+\\.[0-9]+")
        string(REGEX REPLACE "^Version:[ \t]*" "" UTF8PROC_VERSION "${_utf8proc_pc_version}")
    endif()
endif()

find_package_handle_standard_args(Utf8proc
    REQUIRED_VARS UTF8PROC_LIBRARY UTF8PROC_INCLUDE_DIR
    VERSION_VAR   UTF8PROC_VERSION
)

if(Utf8proc_FOUND)
    set(UTF8PROC_INCLUDE_DIRS "${UTF8PROC_INCLUDE_DIR}")
    set(UTF8PROC_LIBRARIES    "${UTF8PROC_LIBRARY}")
endif()

mark_as_advanced(UTF8PROC_INCLUDE_DIR UTF8PROC_LIBRARY)
