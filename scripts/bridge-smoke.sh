#!/usr/bin/env bash
# End-to-end export test: start a real nats-server, monoblok pointed at it
# with an export config, publish locally, assert messages land on the remote.
#
# Requires: nats-server + the nats CLI on PATH.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MONO_PORT="${MONO_PORT:-14222}"
REMOTE_PORT="${REMOTE_PORT:-14888}"
BIN="$ROOT/build/monoblok"
PATCHBAY="/tmp/monoblok-bridge-smoke.edn"

if ! command -v nats-server >/dev/null || ! command -v nats >/dev/null; then
    echo "SKIP: nats-server and/or nats CLI not on PATH"
    exit 0
fi

# Bail loudly if a prior failed run left stuff on our ports — silent
# port contention is worse than a clear error.
for port in "$MONO_PORT" "$REMOTE_PORT"; do
    if lsof -ti ":$port" >/dev/null 2>&1; then
        echo "FAIL: port $port already in use. Kill the holder first, e.g.:"
        echo "  lsof -ti :$port | xargs kill"
        exit 1
    fi
done

if [ ! -x "$BIN" ]; then
    echo "building..."
    (cd "$ROOT" && cmake -S . -B build >/dev/null && cmake --build build --target monoblok) || exit 1
fi

cat > "$PATCHBAY" <<EOF
(export
  :servers ["nats://127.0.0.1:${REMOTE_PORT}"]
  :name    "monoblok-export-smoke"
  :origin-header true
  :export  ["telemetry.>"])

(on "telemetry.*"
  (publish! (subject-append "echoed") payload))
EOF

cleanup() {
    [ -n "${MONO_PID:-}" ] && kill "$MONO_PID" 2>/dev/null || true
    [ -n "${SUB_PID:-}" ] && kill "$SUB_PID" 2>/dev/null || true
    [ -n "${HDR_PID:-}" ] && kill "$HDR_PID" 2>/dev/null || true
    [ -n "${REMOTE_PID:-}" ] && kill "$REMOTE_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -f /tmp/bridge-smoke-*.log "$PATCHBAY"
}
trap cleanup EXIT

echo "starting remote nats-server on :$REMOTE_PORT..."
nats-server -p "$REMOTE_PORT" > /tmp/bridge-smoke-remote.log 2>&1 &
REMOTE_PID=$!

# Wait for nats-server to be ready. It prints "Server is ready" to its log
# once the listener is accepting; poll with a hard cap.
for _ in $(seq 1 50); do
    kill -0 "$REMOTE_PID" 2>/dev/null || break
    grep -q 'Server is ready' /tmp/bridge-smoke-remote.log && break
    sleep 0.1
done
if ! kill -0 "$REMOTE_PID" 2>/dev/null; then
    echo "FAIL: remote nats-server did not start"; cat /tmp/bridge-smoke-remote.log; exit 1
fi
if ! grep -q 'Server is ready' /tmp/bridge-smoke-remote.log; then
    echo "FAIL: remote nats-server did not become ready within 5s"; cat /tmp/bridge-smoke-remote.log; exit 1
fi

echo "subscribing to telemetry.> on remote..."
nats -s "nats://127.0.0.1:${REMOTE_PORT}" sub 'telemetry.>' > /tmp/bridge-smoke-sub.log 2>&1 &
SUB_PID=$!
# Wait for the subscriber to report that it's subscribed.
for _ in $(seq 1 30); do
    grep -q 'Subscribing on' /tmp/bridge-smoke-sub.log && break
    sleep 0.1
done

echo "starting monoblok on :$MONO_PORT with export -> :$REMOTE_PORT..."
"$BIN" --port "$MONO_PORT" --patchbay "$PATCHBAY" > /tmp/bridge-smoke-mono.log 2>&1 &
MONO_PID=$!

# Poll up to 5s for the "bridge: connected" line. The client can take a second
# or two to resolve + connect + exchange INFO even to localhost.
for _ in $(seq 1 50); do
    kill -0 "$MONO_PID" 2>/dev/null || break
    grep -q 'bridge: connected' /tmp/bridge-smoke-mono.log && break
    sleep 0.1
done
if ! kill -0 "$MONO_PID" 2>/dev/null; then
    echo "FAIL: monoblok did not start"; cat /tmp/bridge-smoke-mono.log; exit 1
fi
if ! grep -q 'bridge: connected' /tmp/bridge-smoke-mono.log; then
    echo "FAIL: export client did not connect within 5s"; cat /tmp/bridge-smoke-mono.log; exit 1
fi
echo "ok: export client connected"

(
    printf 'CONNECT {"headers":true}\r\n'
    printf 'SUB telemetry.> 99\r\n'
    printf 'PING\r\n'
    sleep 2
) | nc -w 3 127.0.0.1 "$REMOTE_PORT" > /tmp/bridge-smoke-headers.log &
HDR_PID=$!
sleep 0.2

# Publish two telemetry messages + one unrelated. The rule also republishes
# each telemetry.* as telemetry.*.echoed — both the original and the
# rule-emitted subjects should land on the remote. The unrelated subject
# must NOT land on the remote.
(
    printf 'CONNECT {}\r\n'
    printf 'PUB telemetry.temp 2\r\n11\r\n'
    printf 'PUB telemetry.temp 2\r\n22\r\n'
    printf 'PUB unrelated.x 1\r\na\r\n'
    sleep 0.4
) | nc -w 1 127.0.0.1 "$MONO_PORT" > /dev/null
sleep 0.6

# Sanity: remote saw telemetry.temp + telemetry.temp.echoed, twice each.
OUT=/tmp/bridge-smoke-sub.log
TEMP_COUNT=$(grep -c '^\[.*\] Received on "telemetry\.temp"$' "$OUT" || true)
ECHO_COUNT=$(grep -c '^\[.*\] Received on "telemetry\.temp\.echoed"$' "$OUT" || true)
UNRELATED_COUNT=$(grep -c 'unrelated' "$OUT" || true)

if [ "$TEMP_COUNT" != "2" ]; then
    echo "FAIL: expected 2 telemetry.temp on remote, got $TEMP_COUNT"
    cat "$OUT"; exit 1
fi
if [ "$ECHO_COUNT" != "2" ]; then
    echo "FAIL: expected 2 telemetry.temp.echoed on remote, got $ECHO_COUNT"
    cat "$OUT"; exit 1
fi
if [ "$UNRELATED_COUNT" != "0" ]; then
    echo "FAIL: unrelated.x leaked to remote"
    cat "$OUT"; exit 1
fi
echo "ok: remote received 2x telemetry.temp + 2x telemetry.temp.echoed"
echo "ok: unrelated.x filtered by export filters"

wait "$HDR_PID" 2>/dev/null || true
HEADER_COUNT=$(tr -d '\r' < /tmp/bridge-smoke-headers.log | grep -ci '^x-monoblok: ' || true)
if [ "$HEADER_COUNT" -lt "4" ]; then
    echo "FAIL: expected x-monoblok header on exported messages, got $HEADER_COUNT"
    cat /tmp/bridge-smoke-headers.log; exit 1
fi
echo "ok: exported messages include x-monoblok origin header"

# Spot-check the published counter moved.
(
    printf 'CONNECT {}\r\nSUB \$LVC.\$STATS.bridge.published 1\r\n'
    sleep 0.4
) | nc -w 1 127.0.0.1 "$MONO_PORT" > /tmp/bridge-smoke-stats.log
# $STATS tick is 60s — we don't wait for it here. Just confirm the daemon
# is still healthy and the sub didn't crash it.
if ! kill -0 "$MONO_PID" 2>/dev/null; then
    echo "FAIL: monoblok died"; exit 1
fi
echo "ok: daemon still healthy after exported publishes"

echo
echo "export smoke test passed."
