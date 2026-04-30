#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

if ! command -v nats-server >/dev/null; then
    echo "nats-server is required (https://github.com/nats-io/nats-server)" >&2
    exit 1
fi

REMOTE_PORT=14888
REMOTE_URL="nats://127.0.0.1:$REMOTE_PORT"

nats-server -p $REMOTE_PORT >"$TMP/remote.log" 2>&1 &
REMOTE_PID=$!
EXTRA_PIDS+=("$REMOTE_PID")
sleep 0.4
if ! kill -0 "$REMOTE_PID" 2>/dev/null; then
    echo "nats-server failed to start:"
    cat "$TMP/remote.log"
    exit 1
fi

start_daemon examples/bridge.edn

nats --no-context -s "$REMOTE_URL" sub --raw 'telemetry.>' >"$TMP/remote.txt" 2>/dev/null &
subscribe 'telemetry.>' local.txt
settle 0.4

pub telemetry.troubledwater 1
pub telemetry.troubledwater 2
pub telemetry.troubledwater 3

settle 0.6

note "Three telemetry.troubledwater publishes were sent into monoblok. Each one is delivered locally (and re-emitted under .echoed by a local rule, which also matches telemetry.>), and the bridge forwards every match to the standalone nats-server on port $REMOTE_PORT."
show "publishes"                       _pubs.log
show "local telemetry.> (echoes too)"  local.txt
show "remote telemetry.> (forwarded)"  remote.txt
