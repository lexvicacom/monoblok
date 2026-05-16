#!/usr/bin/env bash
# Build monoblok and bundle it into a release tarball under dist/.
#
# Usage: scripts/package.sh VERSION PLATFORM

set -eu

VERSION="${1:?usage: scripts/package.sh VERSION PLATFORM}"
PLATFORM="${2:?usage: scripts/package.sh VERSION PLATFORM}"
CMAKE_VERSION="${VERSION#v}"
case "$CMAKE_VERSION" in
    *[!0-9.]*|""|.*|*..*|*.) CMAKE_VERSION="0.0.0" ;;
esac

case "$PLATFORM" in
    linux-x86_64|linux-aarch64|macos-aarch64) ;;
    *) echo "unknown PLATFORM: $PLATFORM" >&2; exit 2 ;;
esac

name="monoblok-${VERSION}-${PLATFORM}"

rm -rf build-package
cmake -S . -B build-package -DCMAKE_BUILD_TYPE=Release -DMONOBLOK_VERSION="${CMAKE_VERSION}"
cmake --build build-package --target monoblok

mkdir -p "dist/${name}"
cp build-package/monoblok "dist/${name}/monoblok"
cp scripts/release-README.md "dist/${name}/README.md"
cp patchbay.edn "dist/${name}/"
cp scripts/bench.sh scripts/bench-with-nats-server.sh "dist/${name}/"
case "$PLATFORM" in
    linux-*)
        cp scripts/monoblok.service "dist/${name}/"
        cp scripts/install-systemd.sh "dist/${name}/"
        ;;
esac
cp -r examples "dist/${name}/"
tar -czf "dist/${name}.tar.gz" -C dist "${name}"

echo "packaged: dist/${name}.tar.gz"
