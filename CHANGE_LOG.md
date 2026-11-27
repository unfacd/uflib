# Change Log — uflib

## Release-261127 — 4.2.0
`36682720` → `3c3dd180` · 2026-11-15 → 2026-11-27

### New
- **Flow table** — `adt/flow_table`: a library module keyed by IPv4/IPv6 flow, built on Robin Hood instantiations (`flow4`, `flow6`) under a shared template; the identifier map is now `id_64bit` and exposes iteration
- **InlinedSlab** — an embeddable fixed-size slot pool (`slab/inlined_slab`), with the pool geometry described by `InlinedSlabDescriptor`
- **Collection** — an arena and a descriptor-driven collector returning a `CollectionResult`, plus lazy streams with a fluent facade
- **Hashtable** — init specs, and the configured startup size is honoured on first insertion

### Fixes
- **Install** — the SONAME is taken from the installed object, and superseded `.so` versions are pruned before `ldconfig`
- **Build** — valgrind support is gated on probed capabilities, and a build directory pinned to a scratch companion prefix is rejected
- **Recycler** — slots are reclaimed after a failed checkout
- **Export surface** — the flow and identifier APIs are annotated `PUBLIC_API`

### Deprecated
- (none)

### Removed
- `standard_valgrind_includes.h` — the public valgrind include wrapper; valgrind support is now probed by the build

## Release-261115 — 4.1.0
`59d8a99c` → `ac5afeaf` · 2026-10-04 → 2026-11-15

### New
- **SQLite backend** — opt-in behind `UFLIB_CAPABILITY_SQLITE`: + prepared statements cache. 
- **RFC 6455** — private ip addresses introspection
- **Resource router** — a path-keyed route registry 
- **file_loader** — 

### Fixes
- **file_loader concurrency** — a stale handle is rejected instead of serving the slot's new occupant; a removal racing a load reports `ESTALE`, not `ENOENT`; a node publishes only after the map insert succeeds and the watch is registered first; mappings survive a removal that overtakes a load; a recycled node is reset field by field, and the monitor is reaped once
- **hashtable** — merge and insert take no nested locks, a refused merge says so instead of returning zero, the stored container is resolved before keys are compared, probes are bounded, NULL keys are guarded, and references are counted once

### Deprecated
- (none)

### Removed

## Release-261004 — 4.0.1
`f8a4728d` → `7cc243b5` · 2026-10-01 → 2026-10-03

### New
- `lockless_treiber_stack` gains a closed-state protocol: `steal_all_and_close()` closes the stack as it takes the list, so a push that loses the race is refused with its node still caller-owned; `node_retain()` is added; and `push()` returns `bool` instead of `void`, so callers must handle the result

### Fixes
- Subproject builds: the registry include is `EXISTS`-guarded and defaults to `SYSTEM`, and the CDT source's `<config.h>` — a header uflib neither defines nor generates — is corrected to `<config_uflib.h>`

### Deprecated
- (none)

### Removed
- (none)

## Release-260930 — 4.0.0
`2a9cbfbf` → `f8a4728d` · 2026-09-12 → 2026-09-29

### Capability model for conditional configuration and build of third-party in-source packages

**What changed.** uflib now carries a number of opt-in capability flags, all defaulting
`OFF`. The five in-source packages (K12, lzf, mjson, protobuf-c, zlog) and
utf8proc — previously built unconditionally — are gated, as are the four that
were already opt-in (hiredis, SQL, quirc, qrencode).

**Why it matters.** The default build is now the lean one: no client library, no
vendored package, and **no logger backend**. `UfLoggerCreate` returns
`UF_LOGGER_STATUS_ERR_UNSUPPORTED` until `UFLIB_CAPABILITY_ZLOG=ON`.

**Required consumer action.** Every downstream repo must name what it carries on
uflib's configure line, or it fails at link. Measured usage: ufnetcorelib has 11
files on protobuf-c plus lzf and mjson; ufsrvapi uses K12 and lzf; ufsrvwebsock
uses lzf. **This has not been cycled through the consumers yet.**

**Also closes a hard dependency.** `#include <utf8proc.h>` was unconditional
behind three public helpers, so a build without the header failed and the archive
carried an undefined `utf8proc_iterate` that nothing in uflib's interface named
— the test tree had been finding the library by hand in four separate
`CMakeLists.txt`. With `UFLIB_CAPABILITY_UTF8PROC` off the archive holds zero
utf8proc references.

### Breaking: the public API no longer takes Apple Blocks

`strbufdup_nullable`, `DropRootPrivileges`, `DistinctArrayIterate` and
`DbOpDescriptor.transformer.on_transform` took block parameters, which forced
`-fblocks`.

### New modules

- **logger** — one interface over syslog, stderr, stdout and a rotating file,
- **ufconfig** — configuration api with lua style table syntax which can serialise to redis or sql.
- **QR code** — reading through vendored `quirc`, rendering against system libqrencode.
- **ufcommand** — command registry with alias expansion and keyboard bindings
- **optional** - java-like optional module
