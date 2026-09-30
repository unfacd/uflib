# hiredis (vendored)

## What this is

A vendored copy of **hiredis 0.14.0** — the minimal C client for Redis — built
as a private static library (`hiredis`) for the `ufsrvcli` build.

## Provenance (hard dependency)

| Field | Value |
|---|---|
| Upstream project | [hiredis](https://github.com/redis/hiredis) |
| Version | 0.14.0 (`HIREDIS_MAJOR`/`HIREDIS_MINOR`/`HIREDIS_PATCH` in `hiredis.h`) |
| Copied from | `ufnetcorelib/packages/hiredis/` (the ufsrv shared foundation repo, which itself vendors hiredis) |
| License | BSD-3-Clause (see the per-file copyright headers, e.g. `async.c`) |
| Copied on | 2026-09-05 |

## Why it is vendored

`ufsrvcli` needs a synchronous Redis client (for the `ufsrvscriptlib` Lua
bindings) but does **not** depend on the whole `ufnetcorelib` subtree. Rather
than add that subtree for a single library, we carry a local copy under
`packages/hiredis/`. This is a deliberate hard dependency: these files are
third-party sources and must not be edited in place.

## Update procedure

To refresh, copy the sources again from `ufnetcorelib/packages/hiredis/`:

```bash
cp ufnetcorelib/packages/hiredis/{async.c,async.h,dict.c,dict.h,fmacros.h,hiredis.c,hiredis.h,net.c,net.h,read.c,read.h,sds.c,sds.h,sdsalloc.h,sockcompat.c,sockcompat.h,sslio.c,sslio.h,win32.h} packages/hiredis/
```

`async.c`/`async.h` (the async API) and `sslio.c`/`sslio.h` (SSL, gated by
`HIREDIS_SSL`) are carried for source parity but are unused by the synchronous
bindings; `sslio.h` is still required because `hiredis.c` includes it.
