# uflib

Foundational C library originally written for the **ufsrv** messaging-server platform. It is the
bottom layer of the stack: general-purpose primitives — abstract data types,
lock-free concurrent data structures, an object recycler, crypto/DB/networking
utilities, configuration, logging and a command registry — that every higher
layer builds on. The library is general purpose though and does not make any assumption about where / how it is used.
The lib also embeds external packages, deemed of useful utility, in-source under 'packages' folder. 
These are gated behind so-called capability flags (see [Supported options](#supported-options) for more details).

## Authors
**Ayman Akt** - *and several others as recorded in original contributions which are left intact, mostly under 
packages folder*

Licensed under the GNU Affero General Public License, version 3 or later.

This document describes **uflib 4.0.0.8** — as returned by `UfsrvUfLibVersion()`.

## Installing the packaged library

Debian and Ubuntu users can install uflib without building it. The library is
published as `uflib-dev` from a signed apt repository:

```bash
wget -qO- https://unfacd.github.io/uflib/install.sh | sudo bash
```

That script installs the repository's signing key, adds the apt source, and
installs the package — headers, `libuflib.a`, `uflib.pc` and the CMake package
files. It is non-interactive and safe to re-run, and it requires **`sudo`** (it
writes to `/etc/apt`). It publishes **amd64 only**.

Consume it either way:

```bash
pkg-config --cflags --libs uflib
```

```cmake
find_package(uflib REQUIRED)
target_link_libraries(my_target PRIVATE uflib::uflib)
```

Note that `pkg-config` also needs OpenSSL's development package
(`libssl-dev` on Debian/Ubuntu) — `uflib.pc` declares `Requires: openssl`, which
`libssl3` alone does not provide.

The repository, its signing key and the manual setup steps are documented at
<https://unfacd.github.io/uflib/>. Building from source instead is covered under
[Building](#building).

## Nature and placement under ufsrv stack

uflib is a **standalone, installable library**, not an application fragment.
Dependencies flow strictly downward: it has no visibility of `ufsrvcorelib`,
`ufnetcorelib` or any application server that link them through.

```
ufsrvapi / ufsrvwebsock
  ├──► ufnetcorelib          (unfacd network primitives)
  ├──► ufsrvcorelib          (ufsrv server primitives)
  └──► uflib                 (this library — no upward dependencies)
```

It is consumed by `ufsrvcorelib`, `ufnetcorelib`, `ufsrvapi`, `ufsrvwebsock`,
`ufsrvsfu` and `ufsrvmcp` through:

```cmake
find_package(uflib REQUIRED)
target_link_libraries(my_target PRIVATE uflib::uflib)
```

It builds `libuflib_static.a` and, when `BUILD_SHARED_LIBS=ON`,
`libuflib.so`, both rolled up from a single OBJECT library. The alias
`uflib::uflib` resolves to the shared library if one was built, otherwise the
static one, otherwise the OBJECT library — so it works in every consumption
mode.

It is also embeddable: `add_subdirectory(packages/uflib)` compiles the OBJECT
library directly into the host and skips the install machinery.

## Key modules

Headers are installed under `<uflib/…>`; the table names the subdirectory each
module occupies.

| Area | Headers | What it provides |
|---|---|---|
| **ADTs** | `adt/` | Single-threaded containers: `hashtable_v2`, `hopscotch_hashtable_v2`, `distinct_array`, intrusive singly/doubly linked lists, `queue`, `minheap`, `locking_lru` |
| **CDTs** | `cdt/` | Structures for multi-threaded hot paths: `LamportQueue` (SPSC), `LocklessMpscQueue` (Vyukov), `LocklessFixedWidthHashMap`, `LocklessRingBuffer` (SPSC/MPSC/MPMC), `LocklessLru`, `LocklessMinHeap`, `ChaseLevStealingQueue`, `LocklessTreiberStack`, Anderson and MCS locks |
| **Recycler** | `recycler/`, `recycler_v2/` | Object pooling in slab-like groups, handed out through a lock-free free stack. V2 can CLOCK-evict idle objects to disk and re-materialise them on demand |
| **Buffer descriptor** | `buffer_descriptor/` | Growable string builder used for console and JSON output |
| **Optional** | `optional/` | Owning container for one payload or none: Java `Optional`-style transform, filter, fall back and take, with ownership and failure made explicit |
| **Utilities** | `utils_*.h`, `base32.h`, `tokeniser.h` | String, file, crypto, secrets, time, thread, and encoding helpers (`base64`, `base64url`, `base32`, hex, IEC quantities, URLs, bits, collection-string) |
| **Ufsrv identifier** | `ufsrvuid.h` | The ufsrv-native identifier: a timestamp/instance/sequence tuple with reserved system-user values |
| **Persistence and networking** | `db/`, `net/` | MariaDB/MySQL abstraction; networking helpers |
| **Scheduled jobs** | `scheduled_jobs/` | Scheduled task execution |
| **File loader** | `utils_file_loader.h`, `file_loader_service_concurrent/` | Reload-on-change file caching, and a service that loads files concurrently across registered threads |
| **Logging** | `logger/` | Severity-ranked records through an opaque `UfLogger` handle to syslog, stderr, stdout or a bounded file. The interface names no backend; the library binds one and never reports which |
| **QR code** | `qr_code/` | Reading (vendored `quirc`) and rendering (system `libqrencode`), each behind its own capability |
| **Configuration** | `ufconfig/` | Lua-styled config documents: parse, validate against an injected schema, canonicalise, serve through an opaque handle; optional Redis and SQL drivers and encrypted (`enc:v1:`) field values |
| **Command registry** | `ufcommand/` | Named commands with alias expansion, keyboard bindings, a dispatch pipeline, and opt-in predictive completion and telemetry |

### In-source third-party packages

A number of external library packages are carried verbatim under `packages/`.

| Package | What it is | Licence |
|---|---|---|
| `K12` | KangarooTwelve extendable-output hash (Keccak Team / XKCP) | public domain |
| `lzf` | LZF lossless byte-oriented compression (Marc Lehmann) | BSD 2-clause |
| `mjson` | JSON parser (Cesanta) | MIT |
| `protobuf-c` | Protocol Buffers in C (Dave Benson et al.) | BSD 2-clause |
| `quirc` | QR-code recognition (Daniel Beer) | ISC |
| `zlog` | Thread-safe C logging library, 1.2.18 (Hardy Simpson) | Apache 2.0 |
| `hiredis` | Minimal Redis C client, 0.14.0 | BSD 3-clause |

There are other thirs party packages that have been absorbed into the source tree because they hve undergone some 
significant mods.

## Building

### Requirements

| Requirement | Notes |
|---|---|
| **CMake ≥ 3.20** | Presets require ≥ 3.25 |
| **Clang** | Enforced at configure time by `cmake-modules/ufsrv_build_options.cmake`. The code uses Apple Blocks (`-fblocks`) |
| **clang++** | Only needed to build the C++ test harness |
| C17 with GNU extensions | `-std=gnu17`, `_GNU_SOURCE` |

### Dependencies

| Dependency | How it is resolved |
|---|---|
| **OpenSSL** (`libssl`, `libcrypto`) | `find_package(OpenSSL REQUIRED)` in root mode; in embedded mode from the host repo's linked-libraries registry |
| **MariaDB/MySQL client headers** | Required **unconditionally** to compile `src/db/`. Expected at `/opt/include/mariadb`; both `/opt/include` and `/opt/include/mariadb` are on the include path and are **not** interchangeable — `<mariadb/mysql.h>` and `<mysql.h>` each resolve against only one of them |
| **libmariadb** (the library) | Not vendored and not linked by the default build. Resolved through `cmake/FindMariaDB.cmake` only when `UFLIB_CAPABILITY_SQL=ON`. The DB objects leave unresolved `mysql_*` symbols in `libuflib.a`, so a consumer that links them must supply the client |
| **GoogleTest 1.12.0** | `FetchContent`, only when `_PACKAGE_TESTS=ON`. Cached under `build/_fetchcontent` so it is not re-cloned per build directory |
| **libqrencode** | Only when `UFLIB_CAPABILITY_QRENCODE=ON`, found by `cmake/FindQREncode.cmake` |
| **libutf8proc** | Only when `UFLIB_CAPABILITY_UTF8PROC=ON`, found by `cmake/FindUtf8proc.cmake`. Never named in uflib's exported interface — the consumer links it |
| **Doxygen**, **Valgrind headers** | Optional — API docs target; an extra include path |

The remaining vendored packages — K12, lzf, mjson, protobuf-c, zlog, quirc and
hiredis — are carried in the tree and need nothing installed. Each is built only
under its own capability, and hiredis is never folded into `libuflib.a`: the
consumer links it.

The hard dependency on libmariadb is expected to be resolved in an upcoming release.

### Quick start

```bash
# Configure + build (Debug, no sanitizers)
cmake --preset debug
cmake --build build/debug -j$(nproc)

# The dev preset is the one the fleet installs: ASan + UBSan + LSan, -ggdb3
cmake --preset dev
cmake --build build/dev -j$(nproc)
```

Or configure by hand:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)
```

### Build presets

Presets come from the shared ufsrv set (`CMakePresets.json` →
`cmake-modules/presets/`), byte-identical across the repo family. uflib adds none
of its own. Each configures into `build/<presetName>`.

| Preset | Build type | Sanitizers |
|---|---|---|
| `debug` | Debug | none |
| `release` | Release | none |
| `dev` | Debug | ASan + UBSan + LSan |
| `devtest` | Debug | ASan + UBSan + LSan + coverage |
| `asan`, `msan`, `tsan`, `ubsan` | Debug | the named one |
| `asan-ubsan`, `asan-lsan`, `asan-ubsan-lsan`, `tsan-ubsan`, `msan-ubsan` | Debug | the named combination |

All Debug configurations compile with `-ggdb3` (uflib's own per-repo default,
via `UFSRV_C_DEBUG_FLAGS_DEFAULT`).

```bash
cmake --list-presets -S .     # enumerate
```

### Supported options

| Option | Default | Purpose |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Also build `libuflib.so` alongside `libuflib.a` |
| `_PACKAGE_TESTS` | `OFF` | Build the test harness (gtest suites + stress apps) |
| `_PACKAGE_INSTALL` | `ON` in root mode | Install, pkg-config, CMake package config and CPack targets |
| `UFLIB_CAPABILITY_HIREDIS` | `OFF` | Build the configuration module's Redis driver, and the vendored hiredis client it talks through |
| `UFLIB_CAPABILITY_SQL` | `OFF` | Build the configuration module's SQL driver (needs `find_package(MariaDB)`) |
| `UFLIB_CAPABILITY_QUIRC` | `OFF` | Build the vendored quirc decoder and the QR module's reading half |
| `UFLIB_CAPABILITY_QRENCODE` | `OFF` | Build the QR module's rendering half, against system libqrencode |
| `UFLIB_CAPABILITY_UTF8PROC` | `OFF` | Build the three utf8proc-backed UTF-8 helpers (needs libutf8proc) |
| `UFLIB_CAPABILITY_K12` | `OFF` | Vendor the KangarooTwelve hash |
| `UFLIB_CAPABILITY_LZF` | `OFF` | Vendor the lzf compression library |
| `UFLIB_CAPABILITY_MJSON` | `OFF` | Vendor the mjson parser, and build uflib's `mjson_ex` extension over it |
| `UFLIB_CAPABILITY_PROTOBUF_C` | `OFF` | Vendor the protobuf-c runtime |
| `UFLIB_CAPABILITY_ZLOG` | `OFF` | Build the logger's zlog backend — without it the logger has no backend and `UfLoggerCreate` reports `ERR_UNSUPPORTED` |
| `UFLIB_CAPABILITY_ALL` | `OFF` | Force all ten above on; used by the maintainer scripts |
| `DEV_ENVIRONMENT_AA` | `OFF` | Embedded mode only: host is a ufsrv-provisioned dev environment |
| `UFSRV_BUILD_WITH_{ASAN,MSAN,TSAN}` | `OFF` | Primary sanitizers — mutually exclusive |
| `UFSRV_BUILD_WITH_{UBSAN,LSAN}` | `OFF` | Additive sanitizers |
| `UFSRV_BUILD_WITH_COVERAGE` | `OFF` | gcov/llvm source coverage |
| `UFSRV_VALIDATE_PRESETS` | `ON` | Validate sanitizer exclusivity at configure time |
| `UFSRV_C_DEBUG_FLAGS` | `-ggdb3` | Debug-info flags for the Debug configuration |
| `UFSRV_C_WARNING_FLAGS` | uflib's own list | Warning policy for this repo |

The ten `UFLIB_CAPABILITY_*` options are **capabilities, not detections** — all
default `OFF` and are switched on explicitly. Each appears in the
generated header as a `#cmakedefine`, undefined rather than defined-to-0 when
off, so the header is a complete record of what a build carries.

The default build is therefore the lean one: no client library, no in-source
package. A consumer names what it needs on uflib's
configure line. `UFLIB_CAPABILITY_ALL` is a convenience for the maintainer
scripts, not a capability, and has no macro of its own.

Runtime constants are not CMake options: they are two tiers of preprocessor
defaults in `include/uflib/config_defs_uflib.h` — `CONFIG_DEFAULT_*`
(overridable through a generated `config_uflib.h`) and `PRIV_CONFIG_DEFAULT_*`
(not overridable).

### Useful targets

| Target | What it does |
|---|---|
| `uflib` | The OBJECT library alone |
| `uflib_static` / `uflib_shared` | The roll-ups (`libuflib.a` / `libuflib.so`) |
| `uflib_version` | Regenerate `version.c` with a fresh timestamp |
| `ufsrv_build_digest` | Print the resolved build configuration |
| `check-presets` | Validate sanitizer exclusivity in the preset set |
| `uflib_doxygen` | Generate API docs (if Doxygen is found) |
| `uninstall` / `archive-uninstall` | Remove installed files / archive them to `/tmp` first |
| `install_verification` | Compare version across system install, git HEAD and the local copy |
| `package` | Build the Debian `.deb` (CPack) |

### Installing

To install from the signed apt repository instead of building, see
[Installing the packaged library](#installing-the-packaged-library) — a
one-command `install.sh` covers Debian and Ubuntu.

```bash
cmake --preset dev -DCMAKE_INSTALL_PREFIX=/usr/local -B build/dev
cmake --build build/dev -j$(nproc)
sudo cmake --install build/dev
```

`cmake --install` **copies** the built `libuflib.a`; it does not build it. Make
sure the full build has completed since new sources were added, or the install
will ship a stale archive — verify with `nm /usr/local/lib/libuflib.a | grep <Symbol>`.

For a Debian package the prefix must be `/usr`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_C_COMPILER=clang -D_PACKAGE_TESTS=OFF
cmake --build build -j$(nproc)
cmake --build build --target package
```

## Testing

Tests are gated behind `_PACKAGE_TESTS=ON` (default `OFF`, both standalone and
as a subtree). Production code is C; the test harness is **C++** (GoogleTest
1.12.0 via `FetchContent`).

```bash
cmake --preset dev -B build/dev -D_PACKAGE_TESTS=ON
cmake --build build/dev -j$(nproc)

ctest --test-dir build/dev --output-on-failure -V
ctest --test-dir build/dev -R cdt_fixed_width_hashmap    # one suite
./build/dev/tests/adt/distinct_array/distinct_array_tests # directly, verbosely
```

### Harness layout

- `tests/main.cpp` becomes `gtest_shared_main`, an OBJECT library providing
  `main()` (`InitGoogleTest` → `RUN_ALL_TESTS`) linked into every suite.
- `add_c_test()` in `tests/CMakeLists.txt` wraps a `.cpp` into a gtest
  executable, applies `-fblocks` for C++ (the C headers need it), links
  `GTest::gtest GTest::gtest_main stdc++ m` plus per-module libraries, and
  registers it with CTest.
- Cross the C/C++ boundary with `extern "C" { #include <uflib/...> }`.

### Standalone stress apps

Lock-free CDT modules also build **C** stress executables. They are *not*
registered with CTest because they take CLI arguments:

```bash
./build/dev/tests/cdt/mpsc_queue/cdt_mpsc_queue_stress --threads 8 --duration 5 --nodes 4096
./build/dev/tests/cdt/hashmap/cdt_fixed_width_hashmap_stress --threads 8 --duration 5 --capacity 65536
```

### Race detection

```bash
cmake --preset tsan -B build/tsan -D_PACKAGE_TESTS=ON
cmake --build build/tsan -j$(nproc)
ctest --test-dir build/tsan -R cdt_fixed_width_hashmap
```

### Coverage

`UFSRV_BUILD_WITH_COVERAGE=ON` instruments uflib via
`-fprofile-arcs -ftest-coverage`, on both compile and link lines. Only uflib's
own translation units are instrumented — the vendored packages are exempt.
There is no coverage *target*: the `.gcda` files are produced by running the
tests.

```bash
cmake --preset devtest -B build/devtest -D_PACKAGE_TESTS=ON
cmake --build build/devtest -j$(nproc)
ctest --test-dir build/devtest --output-on-failure

gcovr --gcov-executable "llvm-cov gcov" --object-directory build/devtest \
    --filter 'src/buffer_descriptor/' --print-summary
```

Route coverage tooling through `llvm-cov gcov`. The system `gcov` is GNU gcov
and cannot parse clang's `.gcno` format — it fails with `GCOV returncode was 3`.

### Requirements for new lock-free modules

Any new lock-free concurrent data structure must ship gtest unit tests covering
lifecycle, single-thread correctness, edge cases and at least four concurrent
scenarios, **plus** a standalone multi-threaded stress test with zero-error
assertions. See `CONCURRENT_DATA_STRUCTURES_DESIGN_CONSIDERATIONS.md` here, and
`UF_LIBRARY_CONVENTIONS.md` in the sibling `ufsrv_docs` repo.

### Known issue

In a **fresh** build directory, the `distinct_array_tests` target currently
fails to link:

```
libuflib.a(utils_crypto.c.o): undefined reference to `__b64_ntop'
```

`b64_ntop` comes from glibc's `<resolv.h>` and is not on that target's link:
```bash
cmake -B build -DCMAKE_EXE_LINKER_FLAGS=-lresolv -D_PACKAGE_TESTS=ON …
```

## Further reading

| Document | Scope |
|---|---|
| `docs/man/uflib.7` | Installed manual page: module inventory and API semantics |

## Licence

Copyright © 2015–2026 unfacd works. Distributed under the GNU Affero General
Public License, version 3 or later.
