#!/usr/bin/env bash
# uflib-dev — apt repository bootstrap for Debian/Ubuntu
#
# Usage:  wget -qO- https://unfacd.github.io/uflib/install.sh | sudo bash
#
# Adds the signed unfacd apt repository to this machine and installs uflib-dev.
# There is nothing to download and unpack: apt fetches and verifies the package
# against the repository signature, so this script only has to establish trust
# (the keyring), point apt at the repository, and install.
#
# Because it is piped into bash, stdin IS this script — nothing here may read
# from stdin, and every child process gets </dev/null for the same reason.
set -euo pipefail

REPO_URL="https://unfacd.github.io/uflib"
KEY_URL="$REPO_URL/apt-unfacd.pub.asc"
KEYRING="/etc/apt/keyrings/unfacd-apt.asc"
SOURCES="/etc/apt/sources.list.d/unfacd.sources"
PACKAGE="${PACKAGE:-uflib-dev}"
SUITE="${SUITE:-stable}"

# The signing SUBKEY. Pinned so a tampered key file is caught before it is
# trusted; apt would still refuse a forged package, but this fails earlier and
# names the problem.
EXPECTED_SUBKEY="D808EE4147CC41952C9CBA7B6178496040D97E06"

step() { printf '[%s] %s\n' "$1" "$2"; }
die()  { printf 'ERROR: %s\n' "$1" >&2; exit 1; }

echo "=== uflib-dev installer ==="
echo ""

# ── Preflight ───────────────────────────────────────────────────────────────

[ "$(id -u)" -eq 0 ] || die "must run as root — pipe into 'sudo bash', not 'bash'"

command -v apt-get >/dev/null 2>&1 || die "apt-get not found — this is not a Debian/Ubuntu system"

# Only amd64 is published. Say so plainly rather than letting apt fail with
# "unable to locate package", which reads like a repository problem.
ARCH="$(dpkg --print-architecture 2>/dev/null || echo unknown)"
[ "$ARCH" = "amd64" ] || die "this repository publishes amd64 only; this host is '$ARCH'"

if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    echo "       host: ${PRETTY_NAME:-unknown} ($ARCH)"
fi

for cmd in curl gpg; do
    command -v "$cmd" >/dev/null 2>&1 || die "$cmd is required but not installed"
done

# ── 1. Fetch and verify the signing key ─────────────────────────────────────

step 1/4 "Fetching the repository signing key"
KEY_TMP="$(mktemp)"
trap 'rm -f "$KEY_TMP"' EXIT

curl -fsSL --max-time 30 "$KEY_URL" -o "$KEY_TMP" \
    || die "could not fetch $KEY_URL"

if ! gpg --show-keys --with-colons "$KEY_TMP" 2>/dev/null \
     | awk -F: '/^fpr:/{print $10}' | grep -qx "$EXPECTED_SUBKEY"; then
    echo "       key file fingerprints:" >&2
    gpg --show-keys --with-colons "$KEY_TMP" 2>/dev/null | awk -F: '/^fpr:/{print "         "$10}' >&2
    die "signing subkey $EXPECTED_SUBKEY is NOT in the downloaded key — refusing"
fi
echo "       verified signing subkey $EXPECTED_SUBKEY"

# ── 2. Install the keyring ──────────────────────────────────────────────────

step 2/4 "Installing the keyring"
install -d -m 0755 /etc/apt/keyrings
install -m 0644 "$KEY_TMP" "$KEYRING"
echo "       $KEYRING"

# ── 3. Point apt at the repository ──────────────────────────────────────────

step 3/4 "Adding the repository"
# deb822 format. Idempotent: the file is replaced, never appended to, so
# re-running this script cannot accumulate duplicate stanzas.
cat > "$SOURCES" <<EOF
Types: deb
URIs: $REPO_URL
Suites: $SUITE
Components: main
Signed-By: $KEYRING
EOF
echo "       $SOURCES"

# ── 4. Install ──────────────────────────────────────────────────────────────

step 4/4 "Installing $PACKAGE"
DEBIAN_FRONTEND=noninteractive apt-get update -qq </dev/null \
    || die "apt-get update failed — is the repository reachable?"
DEBIAN_FRONTEND=noninteractive apt-get install -y "$PACKAGE" </dev/null \
    || die "install failed"

INSTALLED="$(dpkg-query -W -f='${Version}' "$PACKAGE" 2>/dev/null || true)"
echo ""

# ── Verify ──────────────────────────────────────────────────────────────────

if [ -z "$INSTALLED" ]; then
    echo "WARNING: $PACKAGE is not registered with dpkg after install." >&2
    exit 1
fi

echo "=== Done — $PACKAGE $INSTALLED ==="
echo ""
echo "Headers:    /usr/include/uflib/"
echo "pkg-config: pkg-config --cflags --libs uflib"
echo "CMake:      find_package(uflib REQUIRED)  ->  target_link_libraries(app PRIVATE uflib::uflib)"
echo "Man page:   man 7 uflib"
echo ""
echo "Repository: $REPO_URL"
