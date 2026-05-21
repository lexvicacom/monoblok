#!/usr/bin/env bash
set -euo pipefail
VERSION=v0.3.5
OS=$(uname -s)
ARCH=$(uname -m)
case "$OS-$ARCH" in
  Darwin-arm64)  PLATFORM=macos-aarch64 ;;
  Darwin-x86_64) PLATFORM=macos-x86_64 ;;
  Linux-x86_64)  PLATFORM=linux-x86_64 ;;
  Linux-aarch64) PLATFORM=linux-aarch64 ;;
  *) echo "Unsupported: $OS-$ARCH"; exit 1 ;;
esac
DIR="monoblok-${VERSION}-${PLATFORM}"
curl -LO "https://github.com/lexvicacom/monoblok/releases/download/${VERSION}/${DIR}.tar.gz"
tar -xzf "${DIR}.tar.gz"
echo
echo "monoblok ${VERSION} downloaded to ./${DIR}/"
echo "run it with:"
echo "  ./${DIR}/monoblok --port 4222 --patchbay ./${DIR}/patchbay.edn"
