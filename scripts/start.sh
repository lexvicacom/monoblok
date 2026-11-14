#!/usr/bin/env bash
set -euo pipefail
VERSION=v0.4.5
OS=$(uname -s)
ARCH=$(uname -m)
case "$OS-$ARCH" in
  Darwin-arm64)  PLATFORM=macos-aarch64 ;;
  Darwin-x86_64) PLATFORM=macos-x86_64 ;;
  Linux-x86_64)  PLATFORM=linux-x86_64 ;;
  Linux-aarch64|Linux-arm64)
    # A Pi can run a 32-bit userspace on a 64-bit kernel.
    if [ "$(getconf LONG_BIT)" = 32 ]; then
      PLATFORM=linux-armv6-gnueabihf
    else
      PLATFORM=linux-aarch64
    fi
    ;;
  Linux-armv6l|Linux-armv7l|Linux-armv8l) PLATFORM=linux-armv6-gnueabihf ;;
  *) echo "Unsupported: $OS-$ARCH"; exit 1 ;;
esac
DIR="monoblok-${VERSION}-${PLATFORM}"
curl -fLO "https://github.com/lexvicacom/monoblok/releases/download/${VERSION}/${DIR}.tar.gz"
tar -xzf "${DIR}.tar.gz"
echo
echo "monoblok ${VERSION} downloaded to ./${DIR}/"
echo "run it with:"
echo "  ./${DIR}/monoblok --port 4222 --patchbay ./${DIR}/patchbay.edn"
