#!/usr/bin/env bash
# End-to-end import test: start a real nats-server, let monoblok import raw
# subjects from it, and assert only patchbay outputs are visible locally.
#
# Requires: nats-server + the nats CLI on PATH.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MONO_PORT="${MONO_PORT:-14223}"
REMOTE_PORT="${REMOTE_PORT:-14889}"
BIN="$ROOT/build/monoblok"
PATCHBAY="/tmp/monoblok-import-smoke.edn"

if ! command -v nats-server >/dev/null || ! command -v nats >/dev/null; then
    echo "SKIP: nats-server and/or nats CLI not on PATH"
    exit 0
fi

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
(import
  :servers ["nats://127.0.0.1:${REMOTE_PORT}"]
  :name    "monoblok-import-smoke"
  :subject ["raw.>"]
  :max-pending 64)

(on "raw.temp"
  (publish! "clean.temp" payload))
EOF

cleanup() {
    [ -n "${MONO_PID:-}" ] && kill "$MONO_PID" 2>/dev/null || true
    [ -n "${CLEAN_SUB_PID:-}" ] && kill "$CLEAN_SUB_PID" 2>/dev/null || true
    [ -n "${RAW_SUB_PID:-}" ] && kill "$RAW_SUB_PID" 2>/dev/null || true
    [ -n "${REMOTE_PID:-}" ] && kill "$REMOTE_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -f /tmp/import-smoke-*.log "$PATCHBAY"
}
trap cleanup EXIT

echo "starting remote nats-server on :$REMOTE_PORT..."
nats-server -p "$REMOTE_PORT" > /tmp/import-smoke-remote.log 2>&1 &
REMOTE_PID=$!

for _ in $(seq 1 50); do
    kill -0 "$REMOTE_PID" 2>/dev/null || break
    grep -q 'Server is ready' /tmp/import-smoke-remote.log && break
    sleep 0.1
done
if ! kill -0 "$REMOTE_PID" 2>/dev/null; then
    echo "FAIL: remote nats-server did not start"; cat /tmp/import-smoke-remote.log; exit 1
fi
if ! grep -q 'Server is ready' /tmp/import-smoke-remote.log; then
    echo "FAIL: remote nats-server did not become ready within 5s"; cat /tmp/import-smoke-remote.log; exit 1
fi

echo "starting monoblok on :$MONO_PORT with import <- :$REMOTE_PORT..."
"$BIN" --port "$MONO_PORT" --patchbay "$PATCHBAY" > /tmp/import-smoke-mono.log 2>&1 &
MONO_PID=$!

for _ in $(seq 1 50); do
    kill -0 "$MONO_PID" 2>/dev/null || break
    grep -q 'import: connected' /tmp/import-smoke-mono.log && break
    sleep 0.1
done
if ! kill -0 "$MONO_PID" 2>/dev/null; then
    echo "FAIL: monoblok did not start"; cat /tmp/import-smoke-mono.log; exit 1
fi
if ! grep -q 'import: connected' /tmp/import-smoke-mono.log; then
    echo "FAIL: import did not connect within 5s"; cat /tmp/import-smoke-mono.log; exit 1
fi
echo "ok: import connected"

nats -s "nats://127.0.0.1:${MONO_PORT}" sub 'clean.>' > /tmp/import-smoke-clean.log 2>&1 &
CLEAN_SUB_PID=$!
nats -s "nats://127.0.0.1:${MONO_PORT}" sub 'raw.>' > /tmp/import-smoke-raw.log 2>&1 &
RAW_SUB_PID=$!
for _ in $(seq 1 30); do
    grep -q 'Subscribing on' /tmp/import-smoke-clean.log &&
        grep -q 'Subscribing on' /tmp/import-smoke-raw.log && break
    sleep 0.1
done

nats -s "nats://127.0.0.1:${REMOTE_PORT}" pub raw.temp 42 >/tmp/import-smoke-pub.log 2>&1
sleep 0.8

CLEAN_COUNT=$(grep -c 'Received on "clean\.temp"' /tmp/import-smoke-clean.log || true)
RAW_COUNT=$(grep -c 'Received on "raw\.temp"' /tmp/import-smoke-raw.log || true)

if [ "$CLEAN_COUNT" != "1" ]; then
    echo "FAIL: expected clean.temp once from imported raw.temp, got $CLEAN_COUNT"
    cat /tmp/import-smoke-clean.log; exit 1
fi
if [ "$RAW_COUNT" != "0" ]; then
    echo "FAIL: imported raw.temp leaked to local raw subscribers"
    cat /tmp/import-smoke-raw.log; exit 1
fi

(
    printf 'CONNECT {}\r\n'
    printf 'PUB raw.temp 2\r\n99\r\n'
    sleep 0.2
) | nc -w 1 127.0.0.1 "$MONO_PORT" > /tmp/import-smoke-local-pub.log
if ! tr -d '\r' < /tmp/import-smoke-local-pub.log | grep -q "Client Publish Disabled"; then
    echo "FAIL: local client PUB was not rejected in import mode"
    cat /tmp/import-smoke-local-pub.log; exit 1
fi

echo "ok: imported raw.temp produced clean.temp"
echo "ok: imported raw.temp stayed private to patchbay"
echo "ok: local client PUB rejected in import mode"
echo
echo "import smoke test passed."
