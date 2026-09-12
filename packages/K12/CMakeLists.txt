# packages/K12/CMakeLists.txt — vendored KangarooTwelve hash (Keccak Team / XKCP)
#
# Only ONE KeccakP-1600 variant may be compiled — every variant defines the
# same public symbols (KeccakP1600_Initialize/_AddByte/_Permute_12rounds/...),
# so including more than one causes a multiple-definition link error.
# include/uflib/K12/KeccakP-1600-SnP.h is the Optimized64 interface (it declares
# _12rounds_FastLoop_Absorb and the opt64_* rename macros), so opt64 is the
# variant in use.
# https://github.com/XKCP/K12

add_library(uflib_k12 STATIC
    Optimized64/KeccakP-1600-opt64.c
    KangarooTwelve.c
)
target_include_directories(uflib_k12
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include/uflib>
)
set_target_properties(uflib_k12 PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Note: ufsrv_apply_build_options intentionally NOT applied — third-party code
