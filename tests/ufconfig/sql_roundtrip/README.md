# SQL round trip

Four pieces, deliberately in two languages, checking that a configuration
survives a trip through MariaDB **and comes back as what was declared**.

```
run.sh              the whole thing against a throwaway container
sql_load.c          document -> store, through the library's own SQL driver
sql_dump.c          store -> json / yaml / ini / lua, through the serialisers
sql_compare.py      store vs document          (the independent reader)
sql_check_dump.py   dump vs store, dump vs document
```

## Why two languages

The C half writes. The Python half reads, through the **server's own client**
(`docker exec … mariadb`), with a parser that shares no code with the C one. A
harness whose reader reuses its writer can show a round trip is *stable*; only
an independent reader can show it is *faithful*. That distinction is the whole
reason this exists rather than a fifth step in `ufconfig_sql_live`.

## Running it

```bash
cmake -B build/gateA_on -DUFLIB_CAPABILITY_SQL=ON …     # the driver must be compiled in
cmake --build build/gateA_on

./tests/ufconfig/sql_roundtrip/run.sh             # everything, then tear down
KEEP=1 ./tests/ufconfig/sql_roundtrip/run.sh      # leave the container up
```

`run.sh` creates a `mariadb:12.3` container (the fleet's version) on
`127.0.0.1:33071`, generates a **root and test password for that run only**,
creates the `ufsrv` database and the `ufsrv` account, runs all four checks, and
removes the container. It does not touch `ufsrv-db`, and it does not use the
deployment key at `/opt/ufsrv/etc/secrets/ufsrv_secret_key` — running these
tests must not require the store they will one day talk to.

It pulls the image only if it is absent. Overrides: `IMAGE`, `CTR`, `PORT`,
`DB`, `TEST_USER`, `DOC`, `BUILD_DIR`, `READY_TIMEOUT`.

By hand, against any server:

```bash
DSN='host=…;port=…;user=…;password=…;database=…'
LOAD=build/gateA_on/tests/ufconfig/sql_roundtrip/ufconfig_sql_load
DUMP=build/gateA_on/tests/ufconfig/sql_roundtrip/ufconfig_sql_dump

$LOAD "$DSN" tests/ufconfig/examples/sample.strict.lua rtcheck ufsrvwebsock
python3 sql_compare.py --document tests/ufconfig/examples/sample.strict.lua \
        --namespace rtcheck --container <ctr> --password-file <file>

$DUMP "$DSN" /tmp/dump rtcheck ufsrvwebsock
python3 sql_check_dump.py --dump-dir /tmp/dump --namespace rtcheck --against store    …
python3 sql_check_dump.py --dump-dir /tmp/dump --namespace rtcheck --against document …
```

The two C programs take the DSN directly. The two Python programs read rows
through `docker exec`, so they take a container name and an in-container port
(3306), not a DSN; the password comes from a file, never a command line.

## Fixtures

| Needed | Where | Notes |
|---|---|---|
| A document | `tests/ufconfig/examples/sample.strict.lua` | 72 paths, three tiers, aliases, inline tables and arrays, and a trailing `return { … }` alias block. Override with `DOC=` |
| The schema | `src/ufconfig/generator/schema/*.lua` | the harnesses generate their own field tables from it at build time |
| The DDL | `src/ufconfig/driver/ufconfig_mariadb.sql` | applied by `sql_load`, idempotently |
| A MariaDB | `run.sh` provides one | 12.3, matching the fleet |
| A build with `UFLIB_CAPABILITY_SQL=ON` | — | otherwise the library has no SQL driver and `run.sh` says so |

Nothing else. No fixture has to be seeded into the store, and the namespace is
removed on the way out — `sql_compare.py` deletes it, including on failure.

## Results to expect

Against `sample.strict.lua`, and these are measurements, not estimates:

| Check | Result |
|---|---|
| `load` | **150 pairs** written for 72 declared paths |
| `compare` (store vs document) | **MATCH 72, DIFFER 0, MISSING 0**, EXTRA 7 |
| store rows | **79** — 150 pairs collapsed, 71 repeated paths, last-wins |
| `dump` | json 2320 B, yaml 2441 B, ini 3336 B, lua 3669 B |
| `check --against store` | JSON 77/79, Lua 73/79 — **DIFFER 0** |
| `check --against document` | JSON matched 70, Lua matched 66 — **DIFFER 0** |
| encrypted fields | 0 in the store, 0 envelopes lost |

The 7 EXTRA are materialised defaults (`testharn.*`, `ufprobe.*`,
`ufsrv.server_id_by_user`) — fields the schema declares that the document does
not, correctly stored and correctly absent from the document side. The
accounting is exact: **72 + 7 = 79**.

### Known reader gaps — read this before treating a failure as a defect

**DIFFER is the signal. MISSING is currently the reader's fault, not the
module's**, and every MISSING in the run above is one of these two:

| Gap | Paths | Why |
|---|---|---|
| Empty tables | `ufsrv.ssl_command_console.allow_list_browser`, `…deny_list` | The dump *does* contain them, as `{}`; but flattening an empty container yields no leaf, while the store holds a real row for it. JSON and Lua both |
| Lua arrays | `…allow_list_god.0/.1`, `ufsrv.fileloader.black_listed_files_registry.0/.1` | The dump's Lua-style array syntax differs from the input document's inline form, and `sql_compare`'s parser does not expand it. JSON indexes them correctly — verified by printing the artefact |

So `run.sh` currently exits **FAIL** on those 8, and the module is fine. Two
fixes would clear it: emit a path for an empty container in `flatten`, and teach
`parse_document` the dump's array form. Until then, treat the MISSING counts as
a known baseline of 2 (JSON) and 6 (Lua) and read DIFFER.

### What neither check has exercised yet

`sql_check_dump.py` asserts that a field the store holds as an `enc:v1:`
envelope must still be an envelope in the dump — the check that would catch a
serialiser reaching for `eff` instead of `raw` and writing a decrypted secret to
disk. **It passed vacuously: the fixture declares no `encrypted = true` field,
so the store held no envelopes.** That assertion is the reason this direction
was built and it is currently untested; a fixture with one encrypted field would
exercise it.

YAML and INI are written and reported but not diffed — there is no YAML parser
in the standard library here, and the INI form is sectioned, so flattening it
would need its own work.
