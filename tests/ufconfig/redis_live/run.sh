#!/usr/bin/env bash
#
# Run the live Redis test against a throwaway Redis.
#
# Nothing here touches the machine it runs on: the container is created for this
# run, published only on loopback, and removed at the end.  It does NOT use any
# of the local Redis containers — redis-session, redis-fence, redis-usrmsg,
# redis-msgq — because those belong to the fleet and a test must not depend on
# them, still less write to them.
#
#   ./run.sh                  the whole thing, and tear down
#   KEEP=1 ./run.sh           leave the container up, print how to reach it
#   BUILD_DIR=build/redis_on  where the binaries are
#
# Requires: docker, and a uflib built with UFLIB_CAPABILITY_HIREDIS=ON — the test links
# the library's Redis driver out of the archive, so the library must carry it.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"

IMAGE=${IMAGE:-redis:6.2-alpine}
CTR=${CTR:-ufconfig-redis-rt}
PORT=${PORT:-63781}
NS=${NS:-ufcfgtest}
BUILD_DIR=${BUILD_DIR:-$repo/build/redis_on}
KEEP=${KEEP:-}
READY_TIMEOUT=${READY_TIMEOUT:-60}

LIVE="$BUILD_DIR/tests/ufconfig/redis_live/ufconfig_redis_live"

command -v docker >/dev/null || { echo "docker is not on PATH"; exit 2; }
[ -x "$LIVE" ] || {
    echo "missing $LIVE"
    echo "build it first:  cmake -B $BUILD_DIR -DUFLIB_CAPABILITY_HIREDIS=ON -D_PACKAGE_TESTS=ON … && cmake --build $BUILD_DIR"
    exit 2
}

# Refuse to clobber a port something else already holds.  A test that quietly
# talked to the fleet's Redis would be worse than one that failed.
if (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep -q ":$PORT "; then
    echo "port $PORT is already in use — set PORT= to a free one"
    exit 2
fi

cleanup() {
    local rc=$?
    if [ -n "$KEEP" ]; then
        echo
        echo "KEEP=1 — container '$CTR' left running on 127.0.0.1:$PORT"
        echo "  docker exec -it $CTR redis-cli"
        echo "  docker rm -f $CTR   # when done"
    else
        docker rm -f "$CTR" >/dev/null 2>&1 || true
    fi
    exit $rc
}
trap cleanup EXIT INT TERM

docker rm -f "$CTR" >/dev/null 2>&1 || true
docker image inspect "$IMAGE" >/dev/null 2>&1 || { echo "pulling $IMAGE …"; docker pull "$IMAGE"; }

echo "ufconfig live Redis test — isolated"
echo "  image       $IMAGE"
echo "  container   $CTR on 127.0.0.1:$PORT"
echo "  namespace   $NS"
echo

# No password: the container is ephemeral and bound to loopback, and
# ufconfig_redis_live takes no credential of its own.  If one is wanted, the
# harness needs a password argument first — UfConfigBackendSpec.redis.password
# exists and the test simply never sets it.
docker run -d --rm --name "$CTR" -p "127.0.0.1:$PORT:6379" "$IMAGE" >/dev/null

printf 'waiting for the server'
ready=0
for _ in $(seq 1 "$READY_TIMEOUT"); do
    if docker exec "$CTR" redis-cli ping 2>/dev/null | grep -q PONG; then
        ready=1
        echo " — ready"
        break
    fi
    printf '.'
    sleep 1
done
if [ "$ready" -ne 1 ]; then
    echo
    echo "the server did not answer in ${READY_TIMEOUT}s; container logs:"
    docker logs "$CTR" 2>&1 | tail -20
    exit 1
fi

echo
"$LIVE" 127.0.0.1 "$PORT" "$NS"
