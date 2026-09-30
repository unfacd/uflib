# UfCommand LLM-agent end-to-end verification protocol

This harness is intended to let an automated coding/review agent verify UfCommand without relying on visual inspection of source code.

## Contract

`tests/golden_e2e.tsv` is a checked-in exact oracle. Every non-comment line is one integration case:

```text
CASE_ID<TAB>SCOPE<TAB>MODE<TAB>INPUT<TAB>EXPECTED
```

The harness reconstructs the host integration fixture, executes the command through the public UfCommand API, captures the handler-visible result, and compares the complete canonical record byte-for-byte.

## Required verification sequence

Tests are opt-in with `_PACKAGE_TESTS=ON`, and every build goes through a preset —
`ufsrv_build_options.cmake` enforces Clang, so a bare `cmake -S . -B build` is not the
supported path. The `dev` preset is the fleet default and already carries
ASan+UBSan+**LSan** (`cmake-modules/presets/ufsrv-sanitizers.json`); `debug` is the same
build without sanitizers. There is no `UFCOMMAND_ENABLE_ASAN` option in this repository.

```sh
# Normal build.
cmake --preset debug -D_PACKAGE_TESTS=ON
cmake --build build/debug -j"$(nproc)"
ctest --test-dir build/debug -R ufcommand --output-on-failure

# Sanitizer build.  The preset configures leak detection; assert it anyway, because
# a protocol that silently stops sanitizing is worse than no protocol.
cmake --preset dev -D_PACKAGE_TESTS=ON
cmake --build build/dev -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
  ctest --test-dir build/dev -R ufcommand --output-on-failure
```

A valid implementation must finish both suites with:

```text
100% tests passed, 0 tests failed
```

and the E2E test must report:

```text
E2E golden: 108 cases, 0 failures
```

The oracle and its config fixture are passed to the harness as absolute paths. Its own
defaults are relative to this package root, so invoking it from anywhere else looks for
the wrong file and exits non-zero for that reason alone.

**One sanitizer report is expected and is not yours.** `UfLoggerCreateWithDefaults()` /
`UfLoggerDestroy()` leak ~121 KB in 16 allocations rooted in the vendored zlog driver
(design document §8.2 #3, owned by `src/logger/`). The CTest targets deliberately create
no logger, so a green CTest run is not hiding it; but `ufcommand_host_app` and anything
else that builds a logger will report it under LSan. Do not attribute it to ufcommand.

## Completion

Two programs cover the completion seam, and they answer different questions.

`ufcommand_completion_tests` is **adversarial** rather than behavioural: it drives the
seam with malformed candidates, non-finite scores, providers that mutate the registry
underneath the engine, a provider that re-enters it, and cursor positions at every token
boundary. It reads the telemetry counters through the module's private header, so its
CMake target adds `${CMAKE_SOURCE_DIR}/src/ufcommand` to the include path.

`ufcommand_completion_golden` is the **exact oracle**, and it uses the public headers
alone — its CMake target deliberately has no private include path. It must report:

```text
golden completion: 48 cases, 0 failures
```

Its corpus is `golden_completion.in` (`case_id<TAB>line<TAB>cursor`) and its expected
output is `golden_completion.out`, one record per case:

```text
count|replacement_start|replacement_end|kind:text;kind:text...
```

**The record carries no score, and that is deliberate.** It asserts what a host can
observe — the candidate set, its order, each kind, and the replacement range — and not the
private constant the dictionary scores with, which the public header never publishes. If
the score is ever published as a contract, the design document's §6.1 records where to put
it back.

### `--record` is a maintainer's tool, never a CI step

The binary accepts `--record GOLDEN_INPUT GOLDEN_OUTPUT` ahead of the other arguments, and
it overwrites the oracle. Do not run it during verification, and never treat a regenerated
file as evidence that the implementation is correct.

Regeneration is not a blind write. Each result is first checked against what the header
promises about *any* result — count within the published cap of 32, replacement range
inside the line, kinds inside the enum, no repeated text, scores non-negative and
non-increasing, and no alias candidate once a complete token precedes the cursor. A build
that violates any of those cannot record a golden at all: `--record` exits non-zero and
writes fewer records than the corpus holds. That is what stops a broken build from baking
its own behaviour in as the expected result, which the byte comparison alone can never
catch.

### The corpus and the oracle both defend themselves

A truncated corpus or oracle (fewer than 40 cases), a duplicated case id, a malformed
corpus row, a cursor that is not a number or is past the end of its line, an oracle record
with no case to consume it, and a line too long for the harness buffer are each a hard
failure. A defect in either checked-in file stops the run at that point rather than
continuing — carrying on would pair the wrong records against each other and bury the real
report under a cascade of mismatches.

## What is covered

The fixture intentionally includes all public command-dispatch seams:

- direct commands
- multi-token command names
- longest-prefix command resolution
- argument cardinality
- aliases
- positional alias substitution
- `$*` alias expansion
- alias chains/cycles
- quoted and escaped command lines
- parser failures
- handler failures
- exact keyboard modifier/key dispatch
- default keyboard modifier dispatch
- persisted user configuration
- save/load round-trip

Both positive and negative cases are present. The corpus includes finite permutations of representative argument values and tokenization forms; “all permutations” therefore means all permutations defined by the checked-in test matrix, rather than an unbounded Cartesian product of arbitrary strings.

## Oracle-integrity check

When modifying the implementation, do not update the expected column merely until the test passes. To validate that the harness actually detects regressions, an agent can make a temporary copy of the golden file, alter one expected status, and verify that `ufcommand_e2e` exits non-zero and identifies the case ID. The same applies to `ufcommand_completion_golden`: copy `golden_completion.out` elsewhere, perturb one record, and confirm it exits non-zero naming that case.

The original checked-in golden files must then remain unchanged unless the documented UfCommand behavior itself is intentionally changed.

## Remediation verification

Do not accept a build as verified unless both the normal and sanitizer CTest runs pass. In particular, the sanitizer run must be configured with leak detection enabled.

The remediation suite specifically guards previously reported defects: alias UAF/cycles/depth, truncated hex escapes, null removal arguments, command-path truncation, namespace conflicts, placeholder grammar, version validation, transactional loading, context lifecycle, dispatch-vs-handler status, binding status, and handler-time registry mutation.
