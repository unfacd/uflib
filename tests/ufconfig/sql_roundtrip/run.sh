#!/usr/bin/env bash
#
# Run the SQL round trip against a throwaway MariaDB.
#
# Nothing in here touches the machine it runs on.  The container is created for
# this run, given credentials generated for this run, and removed at the end —
# so it does not use the fleet's `ufsrv-db`, and it does not use the deployment
# key at /opt/ufsrv/etc/secrets/ufsrv_secret_key.  That is the point: the tests
# are about the module, and running them must not require the store they will
# one day talk to.
#
#   ./run.sh                  the whole thing, and tear down
#   KEEP=1 ./run.sh           leave the container up, print how to reach it
#   BUILD_DIR=build/gateA_on  where the binaries are (default)
#
# Requires: docker, and a uflib built with UFLIB_CAPABILITY_SQL=ON.  Build it with
#   cmake -B build/gateA_on … -DUFLIB_CAPABILITY_SQL=ON
# The image is mariadb:12.3, matching the fleet, and is pulled only if absent.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"

IMAGE=${IMAGE:-mariadb:12.3}
# Not NAME: that is already set in many shells (to the hostname), and a default
# only applies when the variable is unset.
CTR=${CTR:-ufconfig-sql-rt}
PORT=${PORT:-33071}
DB=${DB:-ufsrv}
TEST_USER=${TEST_USER:-ufsrv}
DOC=${DOC:-$repo/tests/ufconfig/examples/sample.strict.lua}
BUILD_DIR=${BUILD_DIR:-$repo/build/gateA_on}
KEEP=${KEEP:-}
READY_TIMEOUT=${READY_TIMEOUT:-90}

LOAD="$BUILD_DIR/tests/ufconfig/sql_roundtrip/ufconfig_sql_load"
DUMP="$BUILD_DIR/tests/ufconfig/sql_roundtrip/ufconfig_sql_dump"

# ── Preconditions ─────────────────────────────────────────────────────────
command -v docker >/dev/null || { echo "docker is not on PATH"; exit 2; }
for b in "$LOAD" "$DUMP"; do
    [ -x "$b" ] || {
        echo "missing $b"
        echo "build it first:  cmake -B $BUILD_DIR -DUFLIB_CAPABILITY_SQL=ON … && cmake --build $BUILD_DIR"
        echo "(or set BUILD_DIR to a tree configured that way)"
        exit 2
    }
done
[ -f "$DOC" ] || { echo "no fixture document at $DOC — set DOC="; exit 2; }

# The tests read rows back with the server's own client, through docker exec.
python3 -c 'import sys' >/dev/null || { echo "python3 is required"; exit 2; }

# ── Credentials, for this run only ────────────────────────────────────────
ROOT_PASS="$(openssl rand -hex 24)"
TEST_PASS="$(openssl rand -hex 24)"
work="$(mktemp -d)"
pwfile="$work/password"
umask 077
printf '%s' "$TEST_PASS" > "$pwfile"
[ -n "$TEST_PASS" ] || { echo "could not generate a password"; exit 2; }

cleanup() {
    local rc=$?
    if [ -n "$KEEP" ]; then
        echo
        echo "KEEP=1 — container '$CTR' left running on 127.0.0.1:$PORT"
        echo "  docker exec -it $CTR mariadb -u $TEST_USER -p'$TEST_PASS' $DB"
        echo "  docker rm -f $CTR   # when done"
        echo "password file kept at $pwfile"
    else
        docker rm -f "$CTR" >/dev/null 2>&1 || true
        rm -rf "$work"
    fi
    exit $rc
}
trap cleanup EXIT INT TERM

# ── The container ─────────────────────────────────────────────────────────
docker rm -f "$CTR" >/dev/null 2>&1 || true
docker image inspect "$IMAGE" >/dev/null 2>&1 || {
    echo "pulling $IMAGE …"
    docker pull "$IMAGE"
}

echo "ufconfig SQL round trip — isolated"
echo "  image       $IMAGE"
echo "  container   $CTR on 127.0.0.1:$PORT"
echo "  database    $DB, account $TEST_USER (generated for this run)"
echo "  document    $DOC"
echo

docker run -d --rm --name "$CTR" \
    -e MARIADB_ROOT_PASSWORD="$ROOT_PASS" \
    -e MARIADB_DATABASE="$DB" \
    -e MARIADB_USER="$TEST_USER" \
    -e MARIADB_PASSWORD="$TEST_PASS" \
    -p "127.0.0.1:$PORT:3306" \
    "$IMAGE" >/dev/null

printf 'waiting for the server'
ready=0
for _ in $(seq 1 "$READY_TIMEOUT"); do
    # Over TCP and as the account the tests use.  `mariadb-admin ping` goes over
    # the unix socket, which answers while the entrypoint's *temporary* server is
    # still initialising — so it reports ready before the port these tests
    # connect to is accepting, and the first connection dies at the handshake.
    if docker exec "$CTR" mariadb -h 127.0.0.1 -P 3306 -u "$TEST_USER" -p"$TEST_PASS" \
            -e 'SELECT 1' >/dev/null 2>&1; then
        ready=1
        echo " — ready"
        break
    fi
    printf '.'
    sleep 1
done
if [ "$ready" -ne 1 ]; then
    echo
    echo "the server did not accept a TCP connection in ${READY_TIMEOUT}s; container logs:"
    docker logs "$CTR" 2>&1 | tail -20
    exit 1
fi

DSN="host=127.0.0.1;port=$PORT;user=$TEST_USER;password=$TEST_PASS;database=$DB"

# The client inside the container reaches the server on its own loopback, so
# the comparison scripts take the container name and the in-container port.
common=(--namespace rtcheck --config ufsrvwebsock
        --container "$CTR" --host 127.0.0.1 --port 3306
        --database "$DB" --user "$TEST_USER" --password-file "$pwfile")

fail=0
step() { echo; echo "── $* ─────────────────────────────────────"; }

# ── Forward: document -> store, read back independently in Python ─────────
step "load: document -> store"
"$LOAD" "$DSN" "$DOC" rtcheck ufsrvwebsock

step "compare: store vs document"
python3 "$here/sql_compare.py" --document "$DOC" "${common[@]}" || fail=1

# ── Reverse: store -> every serialised form ───────────────────────────────
dumpdir="$work/dump"
step "dump: store -> json / yaml / ini / lua"
"$DUMP" "$DSN" "$dumpdir" rtcheck ufsrvwebsock

step "check: dump vs store"
python3 "$here/sql_check_dump.py" --dump-dir "$dumpdir" --against store "${common[@]}" || fail=1

step "check: dump vs document"
python3 "$here/sql_check_dump.py" --dump-dir "$dumpdir" --against document \
        --document "$DOC" "${common[@]}" || fail=1

echo
if [ "$fail" -ne 0 ]; then
    echo "RESULT: FAIL — see the checks above.  MISSING counts are expected for"
    echo "        constructs the reader cannot flatten; see README.md."
    exit 1
fi
echo "RESULT: PASS"
