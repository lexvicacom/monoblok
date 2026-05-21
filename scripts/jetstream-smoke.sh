#!/usr/bin/env bash
# End-to-end JetStream import test: start a real JetStream-enabled nats-server,
# replay two stored messages with a timestamp gap before monoblok opens its
# listener, then verify virtual-clock bar closure and live delivery through the
# same stream import.
#
# Requires: nats-server + the nats CLI on PATH.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MONO_PORT="${MONO_PORT:-15223}"
JS_PORT="${JS_PORT:-15889}"
BRIDGE_PORT="${BRIDGE_PORT:-15890}"
BIN="$ROOT/build/monoblok"
WORKDIR="${TMPDIR:-/tmp}/monoblok-jetstream-smoke"
PATCHBAY="$ROOT/examples/jetstream.yml"
STORE="$WORKDIR/store"
JS_URL="nats://127.0.0.1:${JS_PORT}"
BRIDGE_URL="nats://127.0.0.1:${BRIDGE_PORT}"
MONO_URL="nats://127.0.0.1:${MONO_PORT}"

if ! command -v nats-server >/dev/null || ! command -v nats >/dev/null || ! command -v nc >/dev/null; then
    echo "SKIP: nats-server, nats CLI, and nc not on PATH"
    exit 0
fi

for port in "$MONO_PORT" "$JS_PORT" "$BRIDGE_PORT"; do
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

rm -rf "$WORKDIR"
mkdir -p "$STORE"

cleanup() {
    [ -n "${MONO_PID:-}" ] && kill "$MONO_PID" 2>/dev/null || true
    [ -n "${LVC_SUB_PID:-}" ] && kill "$LVC_SUB_PID" 2>/dev/null || true
    [ -n "${BAR_SUB_PID:-}" ] && kill "$BAR_SUB_PID" 2>/dev/null || true
    [ -n "${BRIDGE_REPLAY_PID:-}" ] && kill "$BRIDGE_REPLAY_PID" 2>/dev/null || true
    [ -n "${BRIDGE_RAW_PID:-}" ] && kill "$BRIDGE_RAW_PID" 2>/dev/null || true
    [ -n "${BRIDGE_HEADER_PID:-}" ] && kill "$BRIDGE_HEADER_PID" 2>/dev/null || true
    [ -n "${LIVE_SUB_PID:-}" ] && kill "$LIVE_SUB_PID" 2>/dev/null || true
    [ -n "${RAW_SUB_PID:-}" ] && kill "$RAW_SUB_PID" 2>/dev/null || true
    [ -n "${JS_PID:-}" ] && kill "$JS_PID" 2>/dev/null || true
    [ -n "${BRIDGE_PID:-}" ] && kill "$BRIDGE_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "starting JetStream nats-server on :$JS_PORT..."
nats-server -js -sd "$STORE" -p "$JS_PORT" > "$WORKDIR/js.log" 2>&1 &
JS_PID=$!
echo "starting bridge target nats-server on :$BRIDGE_PORT..."
nats-server -p "$BRIDGE_PORT" > "$WORKDIR/bridge.log" 2>&1 &
BRIDGE_PID=$!

for _ in $(seq 1 50); do
    kill -0 "$JS_PID" 2>/dev/null || break
    grep -q 'Server is ready' "$WORKDIR/js.log" && break
    sleep 0.1
done
if ! kill -0 "$JS_PID" 2>/dev/null; then
    echo "FAIL: JetStream nats-server did not start"; cat "$WORKDIR/js.log"; exit 1
fi
if ! grep -q 'Server is ready' "$WORKDIR/js.log"; then
    echo "FAIL: JetStream nats-server did not become ready within 5s"; cat "$WORKDIR/js.log"; exit 1
fi
for _ in $(seq 1 50); do
    kill -0 "$BRIDGE_PID" 2>/dev/null || break
    grep -q 'Server is ready' "$WORKDIR/bridge.log" && break
    sleep 0.1
done
if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
    echo "FAIL: bridge target nats-server did not start"; cat "$WORKDIR/bridge.log"; exit 1
fi
if ! grep -q 'Server is ready' "$WORKDIR/bridge.log"; then
    echo "FAIL: bridge target nats-server did not become ready within 5s"; cat "$WORKDIR/bridge.log"; exit 1
fi

nats -s "$JS_URL" stream add SENSORS --subjects "js.sensors.>" --storage file --defaults > "$WORKDIR/stream-add.log" 2>&1
if [ "$?" != "0" ]; then
    echo "FAIL: could not create JetStream stream"; cat "$WORKDIR/stream-add.log"; exit 1
fi
nats -s "$JS_URL" pub js.sensors.temp 41 > "$WORKDIR/history-pub.log" 2>&1
if [ "$?" != "0" ]; then
    echo "FAIL: could not publish historical JetStream message"; cat "$WORKDIR/history-pub.log"; exit 1
fi
sleep 1.2
nats -s "$JS_URL" pub js.sensors.temp 43 > "$WORKDIR/history-pub-2.log" 2>&1
if [ "$?" != "0" ]; then
    echo "FAIL: could not publish second historical JetStream message"; cat "$WORKDIR/history-pub-2.log"; exit 1
fi

nats -s "$BRIDGE_URL" sub 'js.replay.last-temp' > "$WORKDIR/bridge-replay.log" 2>&1 &
BRIDGE_REPLAY_PID=$!
nats -s "$BRIDGE_URL" sub 'js.sensors.temp' > "$WORKDIR/bridge-raw.log" 2>&1 &
BRIDGE_RAW_PID=$!
(
    printf 'CONNECT {"headers":true}\r\n'
    printf 'SUB js.replay.last-temp 19\r\n'
    printf 'PING\r\n'
    sleep 4
) | nc -w 4 127.0.0.1 "$BRIDGE_PORT" > "$WORKDIR/bridge-headers.log" &
BRIDGE_HEADER_PID=$!
for _ in $(seq 1 30); do
    grep -q 'Subscribing on' "$WORKDIR/bridge-replay.log" &&
        grep -q 'Subscribing on' "$WORKDIR/bridge-raw.log" && break
    sleep 0.1
done

echo "starting monoblok on :$MONO_PORT with JetStream import <- :$JS_PORT..."
JS_URL="$JS_URL" BRIDGE_URL="$BRIDGE_URL" "$BIN" --port "$MONO_PORT" --patchbay "$PATCHBAY" > "$WORKDIR/mono.log" 2>&1 &
MONO_PID=$!

for _ in $(seq 1 80); do
    kill -0 "$MONO_PID" 2>/dev/null || break
    grep -q 'jetstream import: connected' "$WORKDIR/mono.log" && break
    sleep 0.1
done
if ! kill -0 "$MONO_PID" 2>/dev/null; then
    echo "FAIL: monoblok did not start"; cat "$WORKDIR/mono.log"; exit 1
fi
if ! grep -q 'jetstream import: connected' "$WORKDIR/mono.log"; then
    echo "FAIL: JetStream import did not connect within 8s"; cat "$WORKDIR/mono.log"; exit 1
fi
echo "ok: JetStream import connected after serial catch-up"

nats -s "$MONO_URL" sub '$LVC.js.replay.last-temp' > "$WORKDIR/lvc.log" 2>&1 &
LVC_SUB_PID=$!
for _ in $(seq 1 30); do
    grep -q 'Received on "\$LVC\.js\.replay\.last-temp"' "$WORKDIR/lvc.log" && break
    sleep 0.1
done
REPLAY_COUNT=$(grep -c 'Received on "\$LVC\.js\.replay\.last-temp"' "$WORKDIR/lvc.log" || true)
if [ "$REPLAY_COUNT" != "1" ] || ! grep -q '^43$' "$WORKDIR/lvc.log"; then
    echo "FAIL: expected js.replay.last-temp LVC value 43 from catch-up"
    cat "$WORKDIR/lvc.log"; exit 1
fi

BRIDGE_REPLAY_COUNT=$(grep -c 'Received on "js\.replay\.last-temp"' "$WORKDIR/bridge-replay.log" || true)
BRIDGE_RAW_COUNT=$(grep -c 'Received on "js\.sensors\.temp"' "$WORKDIR/bridge-raw.log" || true)
if [ "$BRIDGE_REPLAY_COUNT" != "2" ] || ! grep -q '^41$' "$WORKDIR/bridge-replay.log" || ! grep -q '^43$' "$WORKDIR/bridge-replay.log"; then
    echo "FAIL: expected both replay outputs exported to bridge target during catch-up"
    cat "$WORKDIR/bridge-replay.log"; exit 1
fi
BRIDGE_HEADER_COUNT=$(tr -d '\r' < "$WORKDIR/bridge-headers.log" | grep -ci '^x-monoblok: ' || true)
if [ "$BRIDGE_HEADER_COUNT" -lt 1 ]; then
    echo "FAIL: expected x-monoblok header on bridged replay output"
    cat "$WORKDIR/bridge-headers.log"; exit 1
fi
BRIDGE_REPLAY_HEADER_COUNT=$(tr -d '\r' < "$WORKDIR/bridge-headers.log" | grep -ci '^x-monoblok-replay: true' || true)
if [ "$BRIDGE_REPLAY_HEADER_COUNT" -lt 1 ]; then
    echo "FAIL: expected x-monoblok-replay header on bridged replay output"
    cat "$WORKDIR/bridge-headers.log"; exit 1
fi
BRIDGE_TS_HEADER_COUNT=$(tr -d '\r' < "$WORKDIR/bridge-headers.log" | grep -Eci '^x-monoblok-assumed-ts: [0-9]+$' || true)
if [ "$BRIDGE_TS_HEADER_COUNT" -lt 1 ]; then
    echo "FAIL: expected x-monoblok-assumed-ts header on bridged replay output"
    cat "$WORKDIR/bridge-headers.log"; exit 1
fi
if [ "$BRIDGE_RAW_COUNT" != "0" ]; then
    echo "FAIL: JetStream source subject leaked to bridge target"
    cat "$WORKDIR/bridge-raw.log"; exit 1
fi

nats -s "$MONO_URL" sub '$LVC.js.sensors.temp.bar.close' > "$WORKDIR/bar.log" 2>&1 &
BAR_SUB_PID=$!
for _ in $(seq 1 30); do
    grep -q 'Received on "\$LVC\.js\.sensors\.temp\.bar\.close"' "$WORKDIR/bar.log" && break
    sleep 0.1
done
BAR_COUNT=$(grep -c 'Received on "\$LVC\.js\.sensors\.temp\.bar\.close"' "$WORKDIR/bar.log" || true)
if [ "$BAR_COUNT" != "1" ] || ! grep -Eq '^(41|43)$' "$WORKDIR/bar.log"; then
    echo "FAIL: expected replay-time bar close from virtual catch-up clock"
    cat "$WORKDIR/bar.log"; exit 1
fi

nats -s "$MONO_URL" sub 'js.live.temp' > "$WORKDIR/live.log" 2>&1 &
LIVE_SUB_PID=$!
nats -s "$MONO_URL" sub 'js.sensors.temp' > "$WORKDIR/raw.log" 2>&1 &
RAW_SUB_PID=$!
for _ in $(seq 1 30); do
    grep -q 'Subscribing on' "$WORKDIR/live.log" &&
        grep -q 'Subscribing on' "$WORKDIR/raw.log" && break
    sleep 0.1
done

nats -s "$JS_URL" pub js.sensors.temp 42 > "$WORKDIR/live-pub.log" 2>&1
if [ "$?" != "0" ]; then
    echo "FAIL: could not publish live JetStream message"; cat "$WORKDIR/live-pub.log"; exit 1
fi
sleep 1

LIVE_COUNT=$(grep -c 'Received on "js\.live\.temp"' "$WORKDIR/live.log" || true)
RAW_COUNT=$(grep -c 'Received on "js\.sensors\.temp"' "$WORKDIR/raw.log" || true)
BRIDGE_RAW_COUNT=$(grep -c 'Received on "js\.sensors\.temp"' "$WORKDIR/bridge-raw.log" || true)
if [ "$LIVE_COUNT" != "1" ] || ! grep -q '^42$' "$WORKDIR/live.log"; then
    echo "FAIL: expected js.live.temp once from live JetStream message"
    cat "$WORKDIR/live.log"; exit 1
fi
if [ "$RAW_COUNT" != "0" ]; then
    echo "FAIL: JetStream ingress leaked source subject to local subscribers"
    cat "$WORKDIR/raw.log"; exit 1
fi
if [ "$BRIDGE_RAW_COUNT" != "0" ]; then
    echo "FAIL: JetStream source subject leaked to bridge target"
    cat "$WORKDIR/bridge-raw.log"; exit 1
fi

kill "$LVC_SUB_PID" "$BAR_SUB_PID" "$LIVE_SUB_PID" "$RAW_SUB_PID" 2>/dev/null || true
wait "$LVC_SUB_PID" "$BAR_SUB_PID" "$LIVE_SUB_PID" "$RAW_SUB_PID" 2>/dev/null || true
LVC_SUB_PID=
BAR_SUB_PID=
LIVE_SUB_PID=
RAW_SUB_PID=

kill "$MONO_PID" 2>/dev/null || true
wait "$MONO_PID" 2>/dev/null || true
MONO_PID=

echo "restarting monoblok against already-caught-up durable consumer..."
JS_URL="$JS_URL" BRIDGE_URL="$BRIDGE_URL" "$BIN" --port "$MONO_PORT" --patchbay "$PATCHBAY" > "$WORKDIR/mono-second.log" 2>&1 &
MONO_PID=$!
for _ in $(seq 1 80); do
    kill -0 "$MONO_PID" 2>/dev/null || break
    grep -q 'jetstream import: connected' "$WORKDIR/mono-second.log" && break
    sleep 0.1
done
if ! kill -0 "$MONO_PID" 2>/dev/null; then
    echo "FAIL: monoblok did not restart against caught-up durable consumer"; cat "$WORKDIR/mono-second.log"; exit 1
fi
if ! grep -q 'jetstream import: connected' "$WORKDIR/mono-second.log"; then
    echo "FAIL: JetStream import did not reconnect against caught-up durable consumer within 8s"; cat "$WORKDIR/mono-second.log"; exit 1
fi
if grep -q 'Invalid Subscription\|get consumer info failed' "$WORKDIR/mono-second.log"; then
    echo "FAIL: caught-up durable restart used invalid subscription consumer info path"
    cat "$WORKDIR/mono-second.log"; exit 1
fi

echo "ok: replayed historical js.sensors.temp into js.replay.last-temp"
echo "ok: exported replay output to a plain NATS bridge target during catch-up"
echo "ok: bridged replay output included x-monoblok origin, replay, and assumed-ts headers"
echo "ok: closed a replay-time bar from JetStream message timestamps"
echo "ok: delivered live js.sensors.temp into js.live.temp"
echo "ok: JetStream source subject stayed private to patchbay"
echo "ok: restarted cleanly against an already-caught-up durable consumer"
echo
echo "jetstream smoke test passed."
