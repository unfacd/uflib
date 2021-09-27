#the include is limited to one variant (plain64). If optimised is desired, Plain64 must be removed due to conflict in namespace
#see #include "Plain64/KeccakP-1600-SnP.h" "Plain64" must be changed to optimised

#https://github.com/XKCP/K12
set(Inplace32BI_SRCS
        Inplace32BI/KeccakP-1600-inplace32BI.c
)
set(Optimized64_SRCS
        Optimized64/KeccakP-1600-opt64.c
)
set(Plain64_SRCS
        Plain64/KeccakP-1600-plain64.c
)
set(K12_SRCS
        KangarooTwelve.c
)

#[[set(Inplace32BI_SRCS
        Inplace32BI/KeccakP-1600-inplace32BI.c
)

set(Optimized64_SRCS
    Optimized64/KeccakP-1600-AVX2.s
    Optimized64/KeccakP-1600-AVX512.s
    Optimized64/KeccakP-1600-AVX512-plainC.c
    Optimized64/KeccakP-1600-opt64.c
    Optimized64/KeccakP-1600-runtimeDispatch.c
    Optimized64/KeccakP-1600-timesN-AVX2.c
    Optimized64/KeccakP-1600-timesN-AVX512.c
    Optimized64/KeccakP-1600-timesN-SSSE3.c
)

set(Plain64_SRCS
        Plain64/KeccakP-1600-plain64.c
)

set(K12_SRCS
        KangarooTwelve.c
)]]

target_sources(${_PROJECT_NAME} PRIVATE
                 ${Plain64_SRCS} ${K12_SRCS}
        )
