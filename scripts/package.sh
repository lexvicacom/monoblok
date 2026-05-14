#!/usr/bin/env bash
# Build monoblok and bundle it into a release tarball under dist/.
#
# Usage: scripts/package.sh VERSION PLATFORM [VARIANT]
#   VERSION   eg "v0.0.10" or a short sha
#   PLATFORM  eg "linux-x86_64", "linux-aarch64", "macos-aarch64"
#   VARIANT   optional: "default" or "epoll" (Linux only)
#
# Called from .github/workflows/release.yml; safe to run locally too.

set -eu

VERSION="$1"
PLATFORM="$2"
VARIANT="${3:-default}"

case "$VARIANT" in
    default) suffix="" ; build_args="" ;;
    epoll)
        case "$PLATFORM" in
            linux-*) suffix="-epoll"; build_args="-Dforce-epoll=true" ;;
            *) echo "epoll variant is only valid for Linux platforms" >&2; exit 2 ;;
        esac
        ;;
    *) echo "unknown VARIANT: $VARIANT" >&2; exit 2 ;;
esac

# Pin the CPU baseline per platform. Without this Zig defaults to the host
# CPU, and GitHub's ubuntu-22.04-arm runner is a Cobalt-100 (Neoverse-N2-class
# with SVE2), so LLVM auto-vectorizes with SVE and the binary SIGILLs on real
# Neoverse-N1, Graviton2, etc. that lack SVE. neoverse_n1 is ARMv8.2-A + LSE
# + dotprod + crypto with no SVE, a safe lower bound for shipped aarch64.
case "$PLATFORM" in
    linux-x86_64)  target_args="-Dcpu=x86_64_v2" ;;
    linux-aarch64) target_args="-Dcpu=neoverse_n1" ;;
    macos-aarch64) target_args="-Dcpu=apple_m1" ;;
    *) echo "unknown PLATFORM: $PLATFORM" >&2; exit 2 ;;
esac

name="monoblok-${VERSION}-${PLATFORM}${suffix}"

# Fresh zig-out so we don't ship stale bits if a previous run failed halfway.
rm -rf zig-out
zig build --release=safe $target_args $build_args

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
# Tarball on all platforms (no .zip on mac).
tar -czf "dist/${name}.tar.gz" -C dist "${name}"

echo "packaged: dist/${name}.tar.gz"
