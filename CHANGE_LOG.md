# Change Log — uflib

## Unreleased — since `2a9cbfbf` (Release-260912), 2026-09-12 → 2026-09-29
uflib v4.0.0.8
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
