#!/usr/bin/env bash
# Install monoblok as a systemd service on Ubuntu/Debian.
#
# Works from either:
#   * an unpacked release tarball (binary + patchbay + service unit all sit
#     next to this script), or
#   * a repo checkout (binary under build/, patchbay + unit in the repo).
#
# Usage:
#   sudo bash install-systemd.sh [PATH_TO_PATCHBAY]
#
# The binary is always picked up from a known location (tarball-relative
# first, repo-relative second); overriding it is not supported.
#
# Idempotent: safe to re-run to upgrade the binary or refresh the unit.

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "must be run as root (try: sudo $0 ...)" >&2
    exit 1
fi

here="$(cd "$(dirname "$0")" && pwd)"

# Pick the binary. Tarball layout (next to this script) wins; otherwise
# fall back to a repo checkout's build directory.
if [ -x "$here/monoblok" ]; then
    bin_src="$here/monoblok"
elif [ -x "$here/../build/monoblok" ]; then
    bin_src="$here/../build/monoblok"
else
    echo "could not find monoblok binary next to the installer or in build/" >&2
    exit 2
fi

# Pick the patchbay. Explicit arg wins, else tarball, else repo.
if [ "${1:-}" != "" ]; then
    patchbay_src="$1"
elif [ -f "$here/patchbay.edn" ]; then
    patchbay_src="$here/patchbay.edn"
elif [ -f "$here/../patchbay.edn" ]; then
    patchbay_src="$here/../patchbay.edn"
else
    echo "could not find patchbay.edn; pass its path as the first argument" >&2
    exit 2
fi

# Pick the service unit (tarball has it flat; repo has it under scripts/).
if [ -f "$here/monoblok.service" ]; then
    service_src="$here/monoblok.service"
elif [ -f "$here/../scripts/monoblok.service" ]; then
    service_src="$here/../scripts/monoblok.service"
else
    echo "could not find monoblok.service next to this installer" >&2
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

install -m 0644 "$service_src" /etc/systemd/system/monoblok.service
systemctl daemon-reload
echo "installed /etc/systemd/system/monoblok.service"

echo
echo "next steps:"
echo "  sudo systemctl enable --now monoblok"
echo "  journalctl -u monoblok -f          # follow logs"
echo "  systemctl status monoblok"
