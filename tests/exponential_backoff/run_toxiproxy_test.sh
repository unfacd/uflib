#!/usr/bin/env bash
#
# One-invocation entry point for the Toxiproxy DB-reconnect harness.
# Builds the driver, then runs the exhaustion + recovery scenarios end to end
# (provisions a scratch MariaDB + toxiproxy, injects reset_peer, asserts the
# backoff sequence, and tears everything down).
#
# Usage:
#   ./tests/exponential_backoff/run_toxiproxy_test.sh              # both scenarios
#   ./tests/exponential_backoff/run_toxiproxy_test.sh exhaustion   # one scenario
#   ./tests/exponential_backoff/run_toxiproxy_test.sh recovery
#
# Exit 0 if every requested scenario passed, non-zero otherwise.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="$ROOT/build/backoff-test"
GTEST_CACHE="$ROOT/build/_fetchcontent/googletest-src"

SCENARIO="${1:-all}"
case "$SCENARIO" in
  all|exhaustion|recovery|query_error|connect_down) ;;
  *) echo "usage: $0 [all|exhaustion|recovery|query_error|connect_down]" >&2; exit 2 ;;
esac

# ── 1. Configure + build the driver (build-only target, not in CTest) ──────
echo "[run] configuring $BUILD_DIR"
cmake_args=(
  -S "$ROOT"
  -B "$BUILD_DIR"
  -D_PACKAGE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
)
[ -d "$GTEST_CACHE" ] && cmake_args+=(-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$GTEST_CACHE")
cmake "${cmake_args[@]}" >/dev/null

echo "[run] building db_reconnect_driver"
cmake --build "$BUILD_DIR" --target db_reconnect_driver -j"$(nproc)" >/dev/null

# ── 2. Run the scenario(s) ─────────────────────────────────────────────────
run_one() {
  "$HERE/toxiproxy_reconnect.sh" --scenario "$1" \
    --driver "$BUILD_DIR/tests/exponential_backoff/db_reconnect_driver"
}

if [ "$SCENARIO" = "all" ]; then
  run_one exhaustion
  run_one recovery
  run_one query_error
  run_one connect_down
else
  run_one "$SCENARIO"
fi

echo "[run] PASS: toxiproxy reconnect test ($SCENARIO)"
