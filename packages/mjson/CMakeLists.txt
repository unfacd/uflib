# packages/mjson/CMakeLists.txt — vendored mjson JSON parser (Cesanta)
add_library(uflib_mjson STATIC mjson.c)
target_include_directories(uflib_mjson
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include/uflib>
)
set_target_properties(uflib_mjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Note: ufsrv_apply_build_options intentionally NOT applied — third-party code
