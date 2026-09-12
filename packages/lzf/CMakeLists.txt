# packages/lzf/CMakeLists.txt — vendored LZF compression (Marc Lehmann)
add_library(uflib_lzf STATIC lzf_c.c lzf_d.c)
target_include_directories(uflib_lzf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include/uflib>
)
set_target_properties(uflib_lzf PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Note: ufsrv_apply_build_options intentionally NOT applied — third-party code
