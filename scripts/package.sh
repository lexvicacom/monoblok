#!/usr/bin/env bash
# Build monoblok and bundle it into a release tarball under dist/.
#
# Usage: scripts/package.sh VERSION PLATFORM BRIDGE
#   VERSION   eg "v0.0.10" or a short sha
#   PLATFORM  eg "linux-x86_64", "linux-aarch64", "macos-aarch64"
#   BRIDGE    "on" or "off"
#
# Called from .github/workflows/release.yml; safe to run locally too.

set -eu

VERSION="$1"
PLATFORM="$2"
BRIDGE="$3"

case "$BRIDGE" in
    on)  zig_args=""; suffix="" ;;
    off) zig_args="-Dbridge=false"; suffix="-nobridge" ;;
    *)   echo "BRIDGE must be on or off, got '$BRIDGE'" >&2; exit 2 ;;
esac

name="monoblok-${VERSION}-${PLATFORM}${suffix}"

# Fresh zig-out so we don't ship stale bits if a previous run failed halfway.
rm -rf zig-out
zig build --release=safe $zig_args

mkdir -p "dist/${name}"
cp zig-out/bin/monoblok "dist/${name}/"
cp patchbay.edn "dist/${name}/"
cp scripts/bench.sh "dist/${name}/"
# Tarball on all platforms — no .zip on mac.
tar -czf "dist/${name}.tar.gz" -C dist "${name}"

echo "packaged: dist/${name}.tar.gz"
