# install_verification.cmake — uflib informational install check.
#
# Run via the `install_verification` target (cmake -P). Purely informational:
# it (1) compares the library version across three independent sources and
# reports any drift, and (2) introspects the last origin commit for a newly
# added artefact and checks its presence in the installed copy. It never fails
# the build — even a detected mismatch exits 0.
#
#   Source          Origin               Extracted from
#   ------------    ------------------   ----------------------------------------
#   system install  installed library    ${INSTALL_PREFIX}/include/uflib/version.h
#   git             committed HEAD       git show HEAD:include/uflib/version.h
#   local copy      working tree         ${SOURCE_DIR}/include/uflib/version.h
#
# Two version numbers are checked, and they mean different things:
#
#   UFLIB_MAJOR.MINOR.PATCH   the release.  A difference across the three
#                             sources is a real error — sources built against
#                             different releases.
#
#   UFLIB_INTERNAL            the gate counter.  It moves once per approved
#                             gate, not per release, so a working tree ahead of
#                             a commit, or a system install behind one, will
#                             routinely differ.  Reported the same way but not
#                             called drift: a difference here is expected, and
#                             labelling it [DRIFT] beside the release check
#                             would train the reader to ignore both.
#
# Inputs (passed as -D from add_custom_target in CMakeLists.txt):
#   INSTALL_PREFIX  — CMAKE_INSTALL_PREFIX (system install destination)
#   SOURCE_DIR      — CMAKE_SOURCE_DIR (working tree root)
#
# Invoked as:
#   add_custom_target(install_verification
#       COMMAND ${CMAKE_COMMAND}
#           -DINSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
#           -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
#           -P ${CMAKE_SOURCE_DIR}/cmake/install_verification.cmake
#       COMMENT "Verifying uflib version across system install, git, and local copy"
#       VERBATIM)

# ── Guards ─────────────────────────────────────────────────────────────────

if("${SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "install_verification: SOURCE_DIR not set (invoke via the 'install_verification' target)")
endif()
if("${INSTALL_PREFIX}" STREQUAL "")
    set(INSTALL_PREFIX "/usr/local")   # CMake's default install prefix
endif()

# ── Helpers ─────────────────────────────────────────────────────────────────

# parse_version(<content> <out_var>)
# Extracts "MAJOR.MINOR.PATCH" from the text of a version.h file. Falls back
# to "N/A" when the three macros are not all present.
function(parse_version content out_var)
    set(_major "")
    set(_minor "")
    set(_patch "")
    if(content MATCHES "#define[ \t]+UFLIB_MAJOR[ \t]+([0-9]+)")
        set(_major "${CMAKE_MATCH_1}")
    endif()
    if(content MATCHES "#define[ \t]+UFLIB_MINOR[ \t]+([0-9]+)")
        set(_minor "${CMAKE_MATCH_1}")
    endif()
    if(content MATCHES "#define[ \t]+UFLIB_PATCH[ \t]+([0-9]+)")
        set(_patch "${CMAKE_MATCH_1}")
    endif()
    # NOTE: use a non-empty check, not `if(_major AND _minor AND _patch)` — a
    # version component may legitimately be "0", which CMake `if()` treats as
    # false. (uflib's MAJOR is currently non-zero, so the truthiness form worked;
    # this keeps the script correct for any future 0.x checkout.)
    if(NOT "${_major}" STREQUAL "" AND NOT "${_minor}" STREQUAL "" AND NOT "${_patch}" STREQUAL "")
        set(${out_var} "${_major}.${_minor}.${_patch}" PARENT_SCOPE)
    else()
        set(${out_var} "N/A" PARENT_SCOPE)
    endif()
endfunction()

# parse_internal(<content> <out_var>)
# Extracts the incremental UFLIB_INTERNAL gate counter, or "N/A" when the macro
# is absent — a tree predating the counter, which is information rather than an
# error.  Kept separate from parse_version because the two are compared with
# different severity; see the note at the top of this file.
function(parse_internal content out_var)
    if(content MATCHES "#define[ \t]+UFLIB_INTERNAL[ \t]+([0-9]+)")
        set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out_var} "N/A" PARENT_SCOPE)
    endif()
endfunction()

# report_compare(<label> <a> <b>)
# Prints OK when both sides match, DRIFT when they differ, and a neutral
# "unavailable" line when either side could not be determined (N/A).
function(report_compare label a b)
    if("${a}" STREQUAL "N/A" OR "${b}" STREQUAL "N/A")
        message(STATUS "  [  -- ] ${label}: ${a} vs ${b} (source unavailable)")
    elseif("${a}" STREQUAL "${b}")
        message(STATUS "  [  OK ] ${label}: ${a}")
    else()
        message(STATUS "  [DRIFT] ${label}: ${a} vs ${b}")
    endif()
endfunction()

# report_gate(<label> <a> <b>)
# Deliberately NOT report_compare.  A differing gate counter is the ordinary
# state — a working tree sits ahead of the last commit, an install behind it —
# so it is labelled GATE rather than DRIFT.  Calling it drift would put a line
# in this report that is wrong most of the time, and the reader would learn to
# skip the whole block, including the release check that is not.
function(report_gate label a b)
    if("${a}" STREQUAL "N/A" OR "${b}" STREQUAL "N/A")
        message(STATUS "  [  -- ] ${label}: ${a} vs ${b} (source unavailable)")
    elseif("${a}" STREQUAL "${b}")
        message(STATUS "  [  OK ] ${label}: ${a}")
    else()
        message(STATUS "  [GATE] ${label}: ${a} vs ${b} (different gates — expected)")
    endif()
endfunction()

# ── Collect the three versions ─────────────────────────────────────────────

# 1. system install
set(INSTALLED_VERSION "N/A")
set(INSTALLED_INTERNAL "N/A")
set(_installed_header "${INSTALL_PREFIX}/include/uflib/version.h")
if(EXISTS "${_installed_header}")
    file(READ "${_installed_header}" _installed_content)
    parse_version("${_installed_content}" INSTALLED_VERSION)
    parse_internal("${_installed_content}" INSTALLED_INTERNAL)
endif()

# 2. git (committed HEAD)
set(GIT_VERSION "N/A")
set(GIT_INTERNAL "N/A")
find_program(_git_executable git)
if(_git_executable)
    execute_process(
        COMMAND "${_git_executable}" -C "${SOURCE_DIR}" show HEAD:include/uflib/version.h
        RESULT_VARIABLE _git_rc
        OUTPUT_VARIABLE _git_content
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_git_rc EQUAL 0)
        parse_version("${_git_content}" GIT_VERSION)
        parse_internal("${_git_content}" GIT_INTERNAL)
    endif()
endif()

# 3. local copy (working tree)
set(LOCAL_VERSION "N/A")
set(LOCAL_INTERNAL "N/A")
set(_local_header "${SOURCE_DIR}/include/uflib/version.h")
if(EXISTS "${_local_header}")
    file(READ "${_local_header}" _local_content)
    parse_version("${_local_content}" LOCAL_VERSION)
    parse_internal("${_local_content}" LOCAL_INTERNAL)
endif()

# ── Report ─────────────────────────────────────────────────────────────────

message(STATUS "--------------------------------------------------------------")
message(STATUS "uflib install_verification — version drift check (informational)")
message(STATUS "--------------------------------------------------------------")
message(STATUS "  local copy     (working tree)  : ${LOCAL_VERSION}   internal ${LOCAL_INTERNAL}   [${_local_header}]")
message(STATUS "  git HEAD       (committed)     : ${GIT_VERSION}   internal ${GIT_INTERNAL}")
message(STATUS "  system install (${INSTALL_PREFIX}) : ${INSTALLED_VERSION}   internal ${INSTALLED_INTERNAL}   [${_installed_header}]")
message(STATUS "--------------------------------------------------------------")

report_compare("local  vs  git           " "${LOCAL_VERSION}" "${GIT_VERSION}")
report_compare("local  vs  system install" "${LOCAL_VERSION}" "${INSTALLED_VERSION}")
report_compare("git    vs  system install" "${GIT_VERSION}" "${INSTALLED_VERSION}")

# The gate counter, reported on its own line and with its own severity: see the
# note at the top of this file for why a difference here is not called drift.
message(STATUS "  -- UFLIB_INTERNAL (gate counter, not part of the release) --")
report_gate("gate   local  vs  git           " "${LOCAL_INTERNAL}" "${GIT_INTERNAL}")
report_gate("gate   local  vs  system install" "${LOCAL_INTERNAL}" "${INSTALLED_INTERNAL}")
report_gate("gate   git    vs  system install" "${GIT_INTERNAL}" "${INSTALLED_INTERNAL}")

if("${LOCAL_VERSION}" STREQUAL "N/A" OR
   "${GIT_VERSION}" STREQUAL "N/A" OR
   "${INSTALLED_VERSION}" STREQUAL "N/A")
    message(STATUS "=> comparison incomplete — one or more sources unavailable")
elseif("${LOCAL_VERSION}" STREQUAL "${GIT_VERSION}" AND
       "${LOCAL_VERSION}" STREQUAL "${INSTALLED_VERSION}")
    message(STATUS "=> all three sources agree on the release, ${LOCAL_VERSION}")
    if("${LOCAL_INTERNAL}" STREQUAL "${GIT_INTERNAL}" AND
       "${LOCAL_INTERNAL}" STREQUAL "${INSTALLED_INTERNAL}")
        message(STATUS "   and on the gate counter, internal ${LOCAL_INTERNAL}")
    else()
        message(STATUS "   gate counters differ (${LOCAL_INTERNAL} / ${GIT_INTERNAL} / ${INSTALLED_INTERNAL}) — expected between a working tree, a commit and an install")
    endif()
else()
    message(STATUS "=> RELEASE DRIFT detected (see lines above) — informational only, build unaffected")
endif()

# ── Installed library flavours ─────────────────────────────────────────────
# Everything above compares release numbers read from headers. This half reports
# what was actually installed beside them: the static archive and the shared
# object are produced by separate targets from the same objects, so "static,
# shared, or both" and "do the two artefacts carry the same release" are facts
# worth stating, not assuming. Each artefact is asked for the version string its
# own version.c carries, so a stale archive beside a fresh shared object shows
# up here instead of at a consumer's link line.

set(_libdir "${INSTALL_PREFIX}/lib")
set(_static_path "${_libdir}/libuflib.a")
set(_shared_path "")
set(_soname_links "")
file(GLOB _shared_candidates "${_libdir}/libuflib.so*")
foreach(_candidate IN LISTS _shared_candidates)
    get_filename_component(_candidate_name "${_candidate}" NAME)
    # libX.so.M[.m[.p[.internal]]] is the object; libX.so and libX.so.<major> are
    # the links the loader and the linker resolve through. The depth after .so
    # differs per library, so it is matched rather than assumed.
    if(_candidate_name MATCHES "\\.so\\.")
        string(REGEX REPLACE ".*\\.so\\." "" _version_suffix "${_candidate_name}")
        string(REPLACE "." ";" _version_components "${_version_suffix}")
        list(LENGTH _version_components _version_count)
        if(_version_count GREATER 2)
            set(_shared_path "${_candidate}")
        else()
            list(APPEND _soname_links "${_candidate_name}")
        endif()
    else()
        list(APPEND _soname_links "${_candidate_name}")
    endif()
endforeach()

if(EXISTS "${_static_path}" AND NOT "${_shared_path}" STREQUAL "")
    set(_flavour "static + shared")
elseif(EXISTS "${_static_path}")
    set(_flavour "static")
elseif(NOT "${_shared_path}" STREQUAL "")
    set(_flavour "shared")
else()
    set(_flavour "none")
endif()

# The release part of the installed header, and its most significant component.
string(REPLACE "." ";" _release_parts "${INSTALLED_VERSION}")
list(GET _release_parts 0 _installed_major)

# artefact_version(<path> <out_var>)
# Reads the version string the library itself embeds: MAJOR.MINOR.PATCH in some
# libraries, MAJOR.MINOR.PATCH.INTERNAL in others, so the answer is matched
# rather than parsed positionally. Candidates are ranked, because an archive
# also carries its vendored dependencies' version strings — a match on the
# installed release wins over a bare major-version match.
function(artefact_version path out_var)
    set(_found "")
    if(NOT "${path}" STREQUAL "" AND EXISTS "${path}" AND NOT "${_strings_executable}" STREQUAL "_strings_executable-NOTFOUND"
       AND NOT "${INSTALLED_VERSION}" STREQUAL "N/A")
        execute_process(COMMAND "${_strings_executable}" "${path}"
            OUTPUT_VARIABLE _strings_out
            ERROR_QUIET)
        string(REGEX MATCHALL "[0-9]+(\\.[0-9]+)+" _versions "${_strings_out}")
        foreach(_candidate IN LISTS _versions)
            if(_candidate MATCHES "^${INSTALLED_VERSION}$" OR _candidate MATCHES "^${INSTALLED_VERSION}\\.[0-9]+$")
                set(_found "${_candidate}")
                break()
            endif()
        endforeach()
        if("${_found}" STREQUAL "")
            foreach(_candidate IN LISTS _versions)
                if(_candidate MATCHES "^${_installed_major}\\.")
                    set(_found "${_candidate}")
                    break()
                endif()
            endforeach()
        endif()
    endif()
    if("${_found}" STREQUAL "")
        set(${out_var} "N/A" PARENT_SCOPE)
    else()
        set(${out_var} "${_found}" PARENT_SCOPE)
    endif()
endfunction()

# artefact_release(<version> <out_var>)
# The release part — MAJOR.MINOR.PATCH — of an artefact's version string. The
# gate counter is reported by the header checks above and is deliberately not
# compared here: an artefact that embeds only the release is not stale.
function(artefact_release version out_var)
    if("${version}" STREQUAL "N/A")
        set(${out_var} "N/A" PARENT_SCOPE)
        return()
    endif()
    string(REPLACE "." ";" _components "${version}")
    list(LENGTH _components _count)
    if(_count GREATER 2)
        list(SUBLIST _components 0 3 _release_components)
        string(REPLACE ";" "." _release "${_release_components}")
    else()
        set(_release "${version}")
    endif()
    set(${out_var} "${_release}" PARENT_SCOPE)
endfunction()

find_program(_strings_executable strings)

set(_static_version "N/A")
set(_shared_version "N/A")
if(EXISTS "${_static_path}")
    artefact_version("${_static_path}" _static_version)
endif()
if(NOT "${_shared_path}" STREQUAL "")
    artefact_version("${_shared_path}" _shared_version)
endif()
artefact_release("${_static_version}" _static_release)
artefact_release("${_shared_version}" _shared_release)

message(STATUS "--------------------------------------------------------------")
message(STATUS "uflib installed libraries — flavour and artefact agreement")
message(STATUS "--------------------------------------------------------------")
message(STATUS "  install flavour : ${_flavour}")
if(EXISTS "${_static_path}")
    message(STATUS "  static          : ${_static_path}   ${_static_version}")
else()
    message(STATUS "  static          : not installed")
endif()
if(NOT "${_shared_path}" STREQUAL "")
    message(STATUS "  shared          : ${_shared_path}   ${_shared_version}")
    if(NOT "${_soname_links}" STREQUAL "")
        message(STATUS "  soname links    : ${_soname_links}")
    endif()
else()
    message(STATUS "  shared          : not installed")
endif()
message(STATUS "--------------------------------------------------------------")

if(EXISTS "${_static_path}" AND NOT "${_shared_path}" STREQUAL "")
    report_compare("static  vs  shared       " "${_static_release}" "${_shared_release}")
endif()
if(EXISTS "${_static_path}")
    report_compare("header  vs  static       " "${INSTALLED_VERSION}" "${_static_release}")
endif()
if(NOT "${_shared_path}" STREQUAL "")
    report_compare("header  vs  shared       " "${INSTALLED_VERSION}" "${_shared_release}")
endif()

if("${_flavour}" STREQUAL "none")
    message(STATUS "=> nothing installed under ${_libdir} — the install step did not run, or ran elsewhere")
elseif("${_static_version}" STREQUAL "N/A" AND "${_shared_version}" STREQUAL "N/A")
    message(STATUS "=> ${_flavour} installed; artefact versions unreadable (no 'strings', or no header to aim at)")
else()
    message(STATUS "=> ${_flavour} installed; artefacts report static ${_static_version}, shared ${_shared_version}")
endif()
message(STATUS "--------------------------------------------------------------")

# ── Introspection: newly added artefact in the last origin commit ──────────
#
# Looks up the last commit on the origin remote, finds a newly added artefact
# (a header or a .c source), and checks that it is present in the installed
# copy:
#   - header → ${INSTALL_PREFIX}/include/<path>
#   - .c     → its object member in the installed archive
# When the artefact cannot be determined — no origin remote/HEAD, or the last
# commit added no header/source — it reports "introspective verification not
# possible". Optional -DORIGIN_REF=<ref> pins the commit (default: origin HEAD).

function(introspect_origin_artefact source_dir install_prefix)
    find_program(_git git)
    find_program(_ar ar)

    set(_ref "${ORIGIN_REF}")

    # Auto-detect the origin HEAD: origin/HEAD, else origin/master, else origin/main.
    if("${_ref}" STREQUAL "" AND _git)
        foreach(_cand origin/HEAD origin/master origin/main)
            execute_process(
                COMMAND "${_git}" -C "${source_dir}" rev-parse --verify --quiet "${_cand}"
                RESULT_VARIABLE _rc
                OUTPUT_VARIABLE _sha
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(_rc EQUAL 0 AND NOT "${_sha}" STREQUAL "")
                set(_ref "${_cand}")
                break()
            endif()
        endforeach()
    endif()

    if("${_ref}" STREQUAL "" OR NOT _git)
        message(STATUS "  [  -- ] introspective verification not possible (no origin remote/HEAD)")
        return()
    endif()

    # Last commit subject — reported for context.
    execute_process(
        COMMAND "${_git}" -C "${source_dir}" log -1 --format=%s "${_ref}"
        OUTPUT_VARIABLE _subject
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Files ADDED by that commit.
    execute_process(
        COMMAND "${_git}" -C "${source_dir}" diff-tree --root --no-commit-id --diff-filter=A --name-only -r "${_ref}"
        OUTPUT_VARIABLE _added
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REGEX MATCHALL "[^\n]+" _added_files "${_added}")

    # Prefer a header; else a .c source.
    set(_artefact "")
    set(_kind "")
    foreach(_f IN LISTS _added_files)
        if(_f MATCHES "\\.h$")
            set(_artefact "${_f}")
            set(_kind "header")
            break()
        endif()
    endforeach()
    if("${_artefact}" STREQUAL "")
        foreach(_f IN LISTS _added_files)
            if(_f MATCHES "\\.c$")
                set(_artefact "${_f}")
                set(_kind "source")
                break()
            endif()
        endforeach()
    endif()

    if("${_artefact}" STREQUAL "")
        message(STATUS "  [  -- ] introspective verification not possible (last commit added no header/source)")
        message(STATUS "          last origin commit: ${_subject}")
        return()
    endif()

    message(STATUS "  last origin commit '${_subject}' added '${_artefact}'")

    if(_kind STREQUAL "header")
        string(REGEX REPLACE "^include/" "" _rel "${_artefact}")
        set(_installed "${install_prefix}/include/${_rel}")
        if(EXISTS "${_installed}")
            message(STATUS "  [  OK ] header artefact installed: ${_installed}")
        else()
            message(STATUS "  [MISS ] header artefact NOT installed (expected ${_installed})")
        endif()
    else()
        # .c source → its object member must be in the installed archive.
        get_filename_component(_base "${_artefact}" NAME)
        set(_found FALSE)
        foreach(_libname libuflib.a libuflib.so)
            set(_lib "${install_prefix}/lib/${_libname}")
            if(EXISTS "${_lib}" AND _ar)
                execute_process(
                    COMMAND "${_ar}" t "${_lib}"
                    OUTPUT_VARIABLE _members
                    ERROR_QUIET
                )
                string(FIND "${_members}" "${_base}.o" _pos)
                if(_pos GREATER -1)
                    message(STATUS "  [  OK ] source artefact compiled into ${_lib} (${_base}.o)")
                    set(_found TRUE)
                    break()
                endif()
            endif()
        endforeach()
        if(NOT _found)
            message(STATUS "  [MISS ] source artefact not found in installed library (${_artefact})")
        endif()
    endif()
endfunction()

message(STATUS "--------------------------------------------------------------")
message(STATUS "uflib install_verification — origin artefact introspection")
message(STATUS "--------------------------------------------------------------")
introspect_origin_artefact("${SOURCE_DIR}" "${INSTALL_PREFIX}")
message(STATUS "--------------------------------------------------------------")
