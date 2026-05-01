#!/usr/bin/env bash
# End-to-end mixer smoke test. Spawns mixer + 2 workers (T and *), drives
# PUBs over TCP, verifies routing by grepping per-worker stderr.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PORT:-14333}"
BIN="$ROOT/zig-out/bin/monoblok"
TMP="$(mktemp -d /tmp/monoblok-mixer-smoke.XXXXXX)"
MIXER_EDN="$TMP/mixer.edn"
T_EDN="$TMP/t.edn"
DEFAULT_EDN="$TMP/default.edn"
T_LOG="$TMP/t.log"
DEFAULT_LOG="$TMP/default.log"
MIXER_LOG="$TMP/mixer.log"

if [ ! -x "$BIN" ]; then
    echo "building..."
    (cd "$ROOT" && zig build) || exit 1
fi

cleanup() {
    if [ -n "${MIXER_PID:-}" ]; then
        kill "$MIXER_PID" 2>/dev/null || true
        wait "$MIXER_PID" 2>/dev/null || true
    fi
    if [ "${KEEP:-0}" = "1" ]; then
        echo "kept artifacts in $TMP"
    else
        rm -rf "$TMP"
    fi
}
trap cleanup EXIT

# Catch-all rule per worker so --trace prints "subject:" for every PUB.
echo '(on ">" payload)' > "$T_EDN"
echo '(on ">" payload)' > "$DEFAULT_EDN"

cat > "$MIXER_EDN" <<EOF
(mixer
  :listen "tcp://0.0.0.0:$PORT"
  :workers
    ((:shard "T" :patchbay "$T_EDN" :log "$T_LOG" :trace true)
     (:shard "*" :patchbay "$DEFAULT_EDN" :log "$DEFAULT_LOG" :trace true)))
EOF

echo "starting mixer on port $PORT..."
"$BIN" --mixer "$MIXER_EDN" >"$MIXER_LOG" 2>&1 &
MIXER_PID=$!
sleep 0.6

if ! kill -0 "$MIXER_PID" 2>/dev/null; then
    echo "FAIL: mixer did not start"
    cat "$MIXER_LOG"
    exit 1
fi

# Drive two PUBs: T.AAPL.p should land on the T worker, ORDERS.42 on the
# catch-all. We send them on a single nc session so the mixer has to route
# them per-frame, not per-connection.
(
    printf 'CONNECT {}\r\nPUB T.AAPL.p 4\r\n1.23\r\nPUB ORDERS.42 5\r\nhello\r\n'
    sleep 0.4
) | nc -w 2 127.0.0.1 "$PORT" >/dev/null

sleep 0.2

fail=0

if ! grep -q "T.AAPL.p" "$T_LOG"; then
    echo "FAIL: T worker log missing T.AAPL.p"
    echo "---- T_LOG ----"; cat "$T_LOG"
    fail=1
else
    echo "ok: T worker received T.AAPL.p"
fi

if grep -q "T.AAPL.p" "$DEFAULT_LOG"; then
    echo "FAIL: default worker received T.AAPL.p (should have gone to T)"
    fail=1
fi

if ! grep -q "ORDERS.42" "$DEFAULT_LOG"; then
    echo "FAIL: default worker log missing ORDERS.42"
    echo "---- DEFAULT_LOG ----"; cat "$DEFAULT_LOG"
    fail=1
else
    echo "ok: default worker received ORDERS.42"
fi

if grep -q "ORDERS.42" "$T_LOG"; then
    echo "FAIL: T worker received ORDERS.42 (should have gone to default)"
    fail=1
fi

exit $fail
