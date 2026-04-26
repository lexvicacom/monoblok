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

# Pin the CPU baseline per platform. Without this Zig defaults to the host
# CPU, and GitHub's ubuntu-22.04-arm runner is a Cobalt-100 (Neoverse-N2-class
# with SVE2), so LLVM auto-vectorizes with SVE and the binary SIGILLs on real
# Neoverse-N1, Graviton2, etc. that lack SVE. neoverse_n1 is ARMv8.2-A + LSE
# + dotprod + crypto with no SVE, a safe lower bound for shipped aarch64.
#
# Note: -Dcpu only, no -Dtarget. Setting an explicit target triple flips Zig
# into cross-compile mode and disables system library path search, which
# breaks the OpenSSL link the bridge needs. PLATFORM here is just a sanity
# check + tarball-naming concern; the actual target triple comes from the
# host (each CI runner already runs the matching arch natively).
case "$PLATFORM" in
    linux-x86_64)  target_args="-Dcpu=x86_64_v2" ;;
    linux-aarch64) target_args="-Dcpu=neoverse_n1" ;;
    macos-aarch64) target_args="-Dcpu=apple_m1" ;;
    *) echo "unknown PLATFORM: $PLATFORM" >&2; exit 2 ;;
esac

name="monoblok-${VERSION}-${PLATFORM}${suffix}"

# Fresh zig-out so we don't ship stale bits if a previous run failed halfway.
rm -rf zig-out
zig build --release=safe $zig_args $target_args

mkdir -p "dist/${name}"
cp zig-out/bin/monoblok "dist/${name}/"
cp patchbay.edn "dist/${name}/"
cp scripts/bench.sh "dist/${name}/"
# systemd unit + installer are Linux-only.
case "$PLATFORM" in
    linux-*)
        cp scripts/monoblok.service "dist/${name}/"
        cp scripts/install-systemd.sh "dist/${name}/"
        ;;
esac
cp -r examples "dist/${name}/"
# Tarball on all platforms — no .zip on mac.
tar -czf "dist/${name}.tar.gz" -C dist "${name}"

echo "packaged: dist/${name}.tar.gz"
