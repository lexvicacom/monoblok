#!/usr/bin/env bash
# Download a published monoblok release and install it as a systemd service.
#
# Usage: sudo bash install-release.sh VERSION
#   VERSION  eg "v0.0.29"
#
# Pulls the linux-aarch64 tarball from the GitHub release matching VERSION,
# unpacks it into a temp dir, and runs the bundled install-systemd.sh.

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "must be run as root (try: sudo $0 ...)" >&2
    exit 1
fi

if [ "${1:-}" = "" ]; then
    echo "usage: sudo $0 VERSION (eg v0.0.29)" >&2
    exit 2
fi

version="$1"
name="monoblok-${version}-linux-aarch64"
url="https://github.com/lexvicacom/monoblok/releases/download/${version}/${name}.tar.gz"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "downloading $url"
curl -fSL "$url" -o "$tmp/${name}.tar.gz"

tar -xzf "$tmp/${name}.tar.gz" -C "$tmp"

bash "$tmp/${name}/install-systemd.sh"
