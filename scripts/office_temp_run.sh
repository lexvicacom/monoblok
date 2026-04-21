#!/usr/bin/env bash
# Wee end-to-end run of examples/office-temp.edn. Starts monoblok, wires
# a subscriber to temp.1.kitchen.*, replays a scripted publish sequence,
# then prints what the patchbay produced (alert/ok edges, stable stream).
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
MB_BIN="$ROOT/zig-out/bin/monoblok"
PATCHBAY="$ROOT/examples/office-temp.edn"
PORT=4333
export NATS_URL="nats://127.0.0.1:$PORT"

[ -x "$MB_BIN" ] || { echo "build first: zig build --release=fast"; exit 1; }
command -v nats >/dev/null 2>&1 || { echo "missing nats CLI"; exit 1; }

SUB_LOG=/tmp/office_temp_sub.log
MB_LOG=/tmp/office_temp_mb.log
: > "$SUB_LOG"; : > "$MB_LOG"

cleanup() {
    [ -n "${SUB_PID:-}" ] && kill "$SUB_PID" 2>/dev/null
    [ -n "${MB_PID:-}" ] && kill "$MB_PID" 2>/dev/null
    wait 2>/dev/null || true
}
trap cleanup EXIT

"$MB_BIN" --port $PORT --patchbay "$PATCHBAY" > "$MB_LOG" 2>&1 &
MB_PID=$!
sleep 0.3
kill -0 $MB_PID 2>/dev/null || { echo "monoblok failed:"; cat "$MB_LOG"; exit 1; }

# Subscribe to every derived subject the patchbay emits.
nats sub "temp.1.kitchen.>" > "$SUB_LOG" 2>&1 &
SUB_PID=$!
sleep 0.3

pub() { nats pub "temp.1.kitchen" "$1" >/dev/null 2>&1; }

echo "=== replay ==="
echo "warmup (below threshold)"
for v in 12 12 12 13 13.2432 13.2 13.7; do pub "$v"; done

echo "climb: 33 x 65 (crosses moving-avg 60 > 28)"
for _ in $(seq 1 65); do pub 33; done

echo "cool down: 9 x 70 (avg must bleed out the 33s)"
for _ in $(seq 1 70); do pub 9; done

# Let fan-out settle.
sleep 0.5

echo
echo "=== derived messages captured on temp.1.kitchen.> ==="
# nats sub prints multi-line blocks; fold each message to one line.
awk '
    /^\[#[0-9]+\] Received on "/ {
        match($0, /"[^"]+"/); subj = substr($0, RSTART+1, RLENGTH-2); next
    }
    subj && NF > 0 && !/^\[#/ { print subj, "->", $0; subj = "" }
' "$SUB_LOG"

echo
echo "counts by subject:"
awk '
    /^\[#[0-9]+\] Received on "/ { match($0, /"[^"]+"/); print substr($0, RSTART+1, RLENGTH-2) }
' "$SUB_LOG" | sort | uniq -c

echo
echo "done."
