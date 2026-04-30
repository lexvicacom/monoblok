#!/usr/bin/env bash
# End-to-end smoke test: spin up the daemon, drive it over raw TCP with nc,
# verify PUB/SUB + last-value + rules routing.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PORT:-14222}"
SOCK="${SOCK:-/tmp/monoblok-smoke.sock}"
BIN="$ROOT/zig-out/bin/monoblok"
PATCHBAY="$ROOT/patchbay.edn"

if [ ! -x "$BIN" ]; then
    echo "building..."
    (cd "$ROOT" && zig build) || exit 1
fi

cleanup() {
    if [ -n "${DAEMON_PID:-}" ]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -f /tmp/monoblok-smoke-*.txt "$SOCK"
}
trap cleanup EXIT

rm -f "$SOCK"
echo "starting daemon on port $PORT (and unix:$SOCK)..."
"$BIN" --port "$PORT" --unix-socket "$SOCK" --patchbay "$PATCHBAY" >/tmp/monoblok-smoke-daemon.log 2>&1 &
DAEMON_PID=$!
sleep 0.5

if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "FAIL: daemon did not start"
    cat /tmp/monoblok-smoke-daemon.log
    exit 1
fi

SUB_OUT=/tmp/monoblok-smoke-sub.txt
PUB_OUT=/tmp/monoblok-smoke-pub.txt

# --- Test 1: plain SUB does NOT receive any replay --------------------
(
    printf 'CONNECT {}\r\nSUB sensors.temp 1\r\n'
    sleep 0.8
) | nc -w 2 127.0.0.1 "$PORT" > "$SUB_OUT"

if grep -q '^MSG ' "$SUB_OUT"; then
    echo "FAIL: plain SUB should not replay anything"
    echo "---- got ----"
    cat "$SUB_OUT"
    exit 1
fi
echo "ok: plain SUB receives no replay"

# --- Test 1b: \$LVC SUB on empty cache emits no MSG ------------------
(
    printf 'CONNECT {}\r\nSUB $LVC.does.not.exist 2\r\n'
    sleep 0.8
) | nc -w 2 127.0.0.1 "$PORT" > "$SUB_OUT"

if grep -q '^MSG ' "$SUB_OUT"; then
    echo "FAIL: \$LVC on empty cache should emit nothing"
    cat "$SUB_OUT"
    exit 1
fi
echo "ok: \$LVC on empty cache emits nothing"

# --- Test 1c: after a PUB, \$LVC.foo returns cached value -------------
(
    printf 'CONNECT {}\r\nPUB warm.me 5\r\nhello\r\n'
    sleep 0.3
) | nc -w 2 127.0.0.1 "$PORT" >/dev/null
sleep 0.2

(
    printf 'CONNECT {}\r\nSUB $LVC.warm.me 3\r\n'
    sleep 0.8
) | nc -w 2 127.0.0.1 "$PORT" > "$SUB_OUT"

if ! grep -q 'MSG \$LVC.warm.me 3 5' "$SUB_OUT" || ! grep -q '^hello' "$SUB_OUT"; then
    echo "FAIL: \$LVC.warm.me should replay cached value"
    cat "$SUB_OUT"
    exit 1
fi
echo "ok: \$LVC replay returns cached value"

# --- Test 1d: \$LVC wildcard snapshot --------------------------------
(
    printf 'CONNECT {}\r\nSUB $LVC.warm.> 4\r\n'
    sleep 0.8
) | nc -w 2 127.0.0.1 "$PORT" > "$SUB_OUT"

if ! grep -q 'MSG \$LVC.warm.me 4 5' "$SUB_OUT"; then
    echo "FAIL: \$LVC wildcard should emit matching cached entries"
    cat "$SUB_OUT"
    exit 1
fi
echo "ok: \$LVC wildcard snapshot works"

# --- Test 1d2: \$LVC is a LIVE stream: join late, see current, then tail
(
    printf 'CONNECT {}\r\nPUB live.me 2\r\n11\r\nPUB live.me 2\r\n12\r\n'
    sleep 0.3
) | nc -w 2 127.0.0.1 "$PORT" >/dev/null
sleep 0.2

# Subscriber connects after 11 and 12; should get 12 from cache, then 13 live.
(
    printf 'CONNECT {}\r\nSUB $LVC.live.me 6\r\n'
    sleep 1.2
) | nc -w 3 127.0.0.1 "$PORT" > "$SUB_OUT" &
SUB_JOB=$!
sleep 0.4

(
    printf 'CONNECT {}\r\nPUB live.me 2\r\n13\r\n'
    sleep 0.3
) | nc -w 2 127.0.0.1 "$PORT" >/dev/null

wait $SUB_JOB 2>/dev/null || true

if ! grep -qE '^12\r?$' "$SUB_OUT"; then
    echo "FAIL: \$LVC stream should deliver cached value 12 on subscribe"
    cat "$SUB_OUT"; exit 1
fi
if ! grep -qE '^13\r?$' "$SUB_OUT"; then
    echo "FAIL: \$LVC stream should deliver live value 13"
    cat "$SUB_OUT"; exit 1
fi
echo "ok: \$LVC stream delivers cached-then-live values"

# --- Test 1e: PUB to \$LVC is rejected --------------------------------
(
    printf 'CONNECT {}\r\nPUB $LVC.foo 1\r\nx\r\n'
    sleep 0.3
) | nc -w 2 127.0.0.1 "$PORT" > "$PUB_OUT"

if ! grep -q "\-ERR '\$LVC is read-only'" "$PUB_OUT"; then
    echo "FAIL: PUB to \$LVC should be rejected"
    cat "$PUB_OUT"
    exit 1
fi
echo "ok: PUB to \$LVC is rejected"

# --- Test 2: PUB / SUB fan-out with rule firing ----------------------
(
    printf 'CONNECT {}\r\nSUB sensors.> 9\r\n'
    sleep 2.5
) | nc -w 4 127.0.0.1 "$PORT" > "$SUB_OUT" &
SUB_JOB=$!
sleep 0.4

(
    printf 'CONNECT {}\r\nPUB sensors.temp 4\r\n42.5\r\n'
    sleep 0.4
) | nc -w 2 127.0.0.1 "$PORT" > "$PUB_OUT"

wait $SUB_JOB 2>/dev/null || true

if ! grep -q 'MSG sensors.temp 9 4' "$SUB_OUT" || ! grep -q '^42.5' "$SUB_OUT"; then
    echo "FAIL: subscriber did not receive original publish"
    echo "---- got ----"
    cat "$SUB_OUT"
    exit 1
fi
echo "ok: subscriber received sensors.temp publish"

if ! grep -q 'MSG sensors.temp.high 9 4' "$SUB_OUT"; then
    echo "FAIL: rule did not route to sensors.temp.high"
    echo "---- got ----"
    cat "$SUB_OUT"
    exit 1
fi
echo "ok: rule routed 42.5 to sensors.temp.high"

# --- Test 3: alert routing rule --------------------------------------
(
    printf 'CONNECT {}\r\nSUB events.alerts 7\r\n'
    sleep 1.5
) | nc -w 3 127.0.0.1 "$PORT" > "$SUB_OUT" &
SUB_JOB=$!
sleep 0.4

(
    printf 'CONNECT {}\r\nPUB log.app 14\r\nkernel: alert!\r\n'
    sleep 0.4
) | nc -w 2 127.0.0.1 "$PORT" > "$PUB_OUT"

wait $SUB_JOB 2>/dev/null || true

if ! grep -q 'MSG events.alerts 7' "$SUB_OUT"; then
    echo "FAIL: alert rule did not route"
    echo "---- got ----"
    cat "$SUB_OUT"
    exit 1
fi
echo "ok: alert rule routed"

# --- Test 4: threaded pipeline (-> round squelch publish-to) ---------
# Feed 42.01, 42.04, 42.08, 42.12, 43.00 to sensors.temp. The "stable"
# rule is (-> payload-float (round 1) (squelch) (publish-to ...)):
# rounds are 42.0, 42.0, 42.1, 42.1, 43.0 → squelch passes 3 of them.
# publish-to formats numbers canonically: "42", "42.1", "43".
(
    printf 'CONNECT {}\r\nSUB sensors.temp.stable 11\r\n'
    sleep 2.5
) | nc -w 4 127.0.0.1 "$PORT" > "$SUB_OUT" &
SUB_JOB=$!
sleep 0.4

(
    for v in 42.01 42.04 42.08 42.12 43.00; do
        printf 'PUB sensors.temp %d\r\n%s\r\n' "${#v}" "$v"
    done
    # Include CONNECT so the daemon accepts the session; prepend it.
    :
) | (
    printf 'CONNECT {}\r\n'
    cat
    sleep 0.6
) | nc -w 3 127.0.0.1 "$PORT" > "$PUB_OUT"

wait $SUB_JOB 2>/dev/null || true

STABLE_COUNT=$(grep -c '^MSG sensors.temp.stable 11 ' "$SUB_OUT" || true)
if [ "$STABLE_COUNT" != "3" ]; then
    echo "FAIL: expected 3 MSG on sensors.temp.stable, got $STABLE_COUNT"
    echo "---- got ----"
    cat "$SUB_OUT"
    exit 1
fi
# Strip \r for a stable payload check; expect exactly these three payloads.
PAYLOADS=$(tr -d '\r' < "$SUB_OUT" | awk '/^MSG sensors\.temp\.stable/ { getline; print }')
EXPECTED=$'42\n42.1\n43'
if [ "$PAYLOADS" != "$EXPECTED" ]; then
    echo "FAIL: threaded pipeline payload mismatch"
    echo "---- expected ----"
    echo "$EXPECTED"
    echo "---- got ----"
    echo "$PAYLOADS"
    exit 1
fi
echo "ok: threaded pipeline emitted rounded+squelched values"

# --- Test 5: unix-socket listener (cross-listener fan-out) ------------
# TCP subscriber should receive a publish that arrives over the unix socket.
(
    printf 'CONNECT {}\r\nSUB unix.demo 21\r\n'
    sleep 1.5
) | nc -w 3 127.0.0.1 "$PORT" > "$SUB_OUT" &
SUB_JOB=$!
sleep 0.4

(
    printf 'CONNECT {}\r\nPUB unix.demo 5\r\nhello\r\n'
    sleep 0.3
) | nc -U -w 2 "$SOCK" > "$PUB_OUT"

wait $SUB_JOB 2>/dev/null || true

if ! grep -q 'MSG unix.demo 21 5' "$SUB_OUT" || ! grep -q '^hello' "$SUB_OUT"; then
    echo "FAIL: PUB on unix socket should reach TCP subscriber"
    echo "---- got ----"
    cat "$SUB_OUT"
    exit 1
fi
echo "ok: unix-socket PUB reaches TCP subscriber"

echo
echo "all smoke tests passed."
