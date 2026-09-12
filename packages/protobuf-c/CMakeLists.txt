# packages/protobuf-c/CMakeLists.txt — vendored protobuf-c implementation
add_library(uflib_protobuf_c STATIC protobuf-c.c)
target_include_directories(uflib_protobuf_c
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include/uflib>
)
set_target_properties(uflib_protobuf_c PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Note: ufsrv_apply_build_options intentionally NOT applied — third-party code
