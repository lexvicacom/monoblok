#!/usr/bin/env bash
# Install monoblok as a systemd service on Ubuntu/Debian.
#
# Usage (from a release tarball or repo checkout containing the binary):
#   sudo bash scripts/install-systemd.sh [PATH_TO_BINARY] [PATH_TO_PATCHBAY]
#
# Defaults:
#   PATH_TO_BINARY   = ./zig-out/bin/monoblok (or ./monoblok if present)
#   PATH_TO_PATCHBAY = ./patchbay.edn
#
# Idempotent: safe to re-run to upgrade the binary or refresh the unit.

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "must be run as root (try: sudo $0 ...)" >&2
    exit 1
fi

here="$(cd "$(dirname "$0")" && pwd)"

# Pick the binary.
if [ "${1:-}" != "" ]; then
    bin_src="$1"
elif [ -x "$here/../zig-out/bin/monoblok" ]; then
    bin_src="$here/../zig-out/bin/monoblok"
elif [ -x "$here/../monoblok" ]; then
    bin_src="$here/../monoblok"
else
    echo "could not find monoblok binary; pass its path as the first argument" >&2
    exit 2
fi

patchbay_src="${2:-$here/../patchbay.edn}"
if [ ! -f "$patchbay_src" ]; then
    echo "patchbay file not found: $patchbay_src" >&2
    exit 2
fi

# Service user (system account, no shell, no home dir).
if ! id monoblok >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin monoblok
    echo "created user 'monoblok'"
fi

install -m 0755 "$bin_src" /usr/local/bin/monoblok
echo "installed /usr/local/bin/monoblok"

install -d -m 0755 /etc/monoblok
# Don't clobber an existing config on upgrade; ship a .new for the operator to diff.
if [ -f /etc/monoblok/patchbay.edn ]; then
    install -m 0644 "$patchbay_src" /etc/monoblok/patchbay.edn.new
    echo "kept existing /etc/monoblok/patchbay.edn; new version at patchbay.edn.new"
else
    install -m 0644 "$patchbay_src" /etc/monoblok/patchbay.edn
    echo "installed /etc/monoblok/patchbay.edn"
fi

install -m 0644 "$here/monoblok.service" /etc/systemd/system/monoblok.service
systemctl daemon-reload
echo "installed /etc/systemd/system/monoblok.service"

echo
echo "next steps:"
echo "  sudo systemctl enable --now monoblok"
echo "  journalctl -u monoblok -f          # follow logs"
echo "  systemctl status monoblok"
