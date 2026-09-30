# packages/quirc/CMakeLists.txt — vendored quirc QR-code recognition library
#
# quirc -- QR-code recognition library
# Copyright (C) 2010-2012 Daniel Beer <dlbeer@gmail.com>
# ISC License — see LICENSE beside this file, and the per-file headers.
# Upstream: https://github.com/dlbeer/quirc
#
# Vendored at 927d680904dc95fdff4cd9d022eb374b438ff8f2 (v1.2-7-g927d680, 2025-05-20).
# quirc_version() returns the literal "1.0" — upstream never bumped it — so the commit
# above, not that string, is the provenance version.
#
# Flattened: the four translation units upstream's Makefile builds into libquirc.a, plus
# quirc_internal.h.  demo/ and tests/ are pruned — SDL, V4L2, libjpeg, libpng, OpenCV.
#
# This file is reached only from behind `if(UFLIB_CAPABILITY_QUIRC)` in the top-level
# CMakeLists.txt, so nothing here needs a gate of its own.

add_library(uflib_quirc STATIC
    quirc.c
    identify.c
    decode.c
    version_db.c
)

# quirc.h is public-facing and lives in the installed tree at include/uflib/quirc/.
# quirc_internal.h stays here and reaches it by basename, so this directory's own
# sources need the public location on their include path.  UFLIB_QUIRC_PUBLIC_INCLUDE_DIR
# is set by the parent — inside the gate, so nothing acquires that path unless quirc
# was asked for.
target_include_directories(uflib_quirc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE
        ${UFLIB_QUIRC_PUBLIC_INCLUDE_DIR}
)

# identify.o is the only object in the archive with libm relocations, and it has
# three: rint at identify.c:107 and :108, sqrt at :659.  (The same file calls fabs
# at :1022 and :1051, but the compiler lowers that to bit manipulation and it never
# becomes an undefined symbol.)  On glibc these are libm's, not libc's.  INTERFACE
# rather than PRIVATE because a static archive has no link line of its own: whoever
# extracts identify.o is who must reach libm.
target_link_libraries(uflib_quirc INTERFACE m)

set_target_properties(uflib_quirc PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Note: ufsrv_apply_build_options intentionally NOT applied — third-party code
