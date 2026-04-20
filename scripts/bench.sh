#!/usr/bin/env bash
# Bench monoblok and nats-server on the current host. Same workload is
# driven against both, one at a time — not concurrently.
#
# Honours NATS_URL (same env var the nats CLI reads) for the address;
# defaults to nats://127.0.0.1:4222. Both servers bind to the same port
# but are started in sequence.
#
# Requirements:
#   - nats CLI     (https://github.com/nats-io/natscli/releases)
#   - nats-server  (optional; comparison section skipped if absent)
#
# Usage: ./scripts/bench.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
# Binary lives next to the script on deployed boxes (dist/<triple>/);
# fall back to zig-out/bin when run out of a source checkout.
if [ -x "$HERE/monoblok" ]; then
    MB_BIN="$HERE/monoblok"
    ROOT="$HERE"
else
    ROOT="$(cd "$HERE/.." && pwd)"
    MB_BIN="$ROOT/zig-out/bin/monoblok"
fi

export NATS_URL="${NATS_URL:-nats://127.0.0.1:4222}"
# Extract host:port → port for the server --port flag.
PORT="${NATS_URL##*:}"

command -v nats >/dev/null 2>&1 || { echo "missing: nats CLI (install from https://github.com/nats-io/natscli/releases)"; exit 1; }
if ! command -v nats-server >/dev/null 2>&1; then
    echo "warning: nats-server not installed — comparison section will be skipped"
fi

if [ ! -x "$MB_BIN" ]; then
    echo "building..."
    (cd "$ROOT" && zig build --release=safe) || exit 1
fi

cleanup() {
    [ -n "${MB_PID:-}" ] && kill "$MB_PID" 2>/dev/null && wait "$MB_PID" 2>/dev/null || true
    [ -n "${NS_PID:-}" ] && kill "$NS_PID" 2>/dev/null && wait "$NS_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Extract the overall msgs/sec from a nats-bench output stream. Handles
# every format we've seen:
#   v0.2: "Pub stats: 1,234 msgs/sec" / "Sub stats: ..."
#   v0.3 single-client: "... publisher stats: 1,234 msgs/sec ..."
#   v0.3 multi-client:  "... publisher aggregated stats: 1,234 msgs/sec ..."
extract_rate() {
    local kind="$1" # "publisher" or "subscriber"
    awk -v kind="$kind" '
        ($0 ~ "stats:") && ($0 ~ kind || $0 ~ "Pub stats" || $0 ~ "Sub stats") {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^[0-9,]+$/ && $(i+1) == "msgs/sec") { print $i; exit }
            }
        }
    '
}

run_pub() {
    local clients="$1" msgs="$2" size="$3"
    nats bench pub --clients=$clients --msgs=$msgs --size=$size solo 2>&1 \
        | extract_rate publisher
}

run_fanout() {
    local subs="$1" msgs="$2"
    nats bench sub --clients=$subs --msgs=$msgs fan > /tmp/bench_sub.out 2>&1 &
    local sub_pid=$!
    sleep 0.8
    nats bench pub --clients=1 --msgs=$msgs --size=64 fan > /dev/null 2>&1
    wait $sub_pid 2>/dev/null || true
    extract_rate subscriber < /tmp/bench_sub.out
}

run_sweep() {
    printf "%-30s %s msgs/sec\n" "1 pub × 500k × 64B"  "$(run_pub 1 500000 64)"
    printf "%-30s %s msgs/sec\n" "2 pub × 10k × 64B"   "$(run_pub 2 10000 64)"
    printf "%-30s %s msgs/sec\n" "8 pub × 50k × 128B"  "$(run_pub 8 50000 128)"
    printf "%-30s %s msgs/sec\n" "1 pub → 1 sub"       "$(run_fanout 1 200000)"
    printf "%-30s %s msgs/sec\n" "1 pub → 10 subs"     "$(run_fanout 10 50000)"
    printf "%-30s %s msgs/sec\n" "1 pub → 50 subs"     "$(run_fanout 50 20000)"
}

echo "=== host ==="
uname -a
if command -v nproc >/dev/null 2>&1; then
    echo "cpu cores: $(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    echo "cpu cores: $(sysctl -n hw.ncpu 2>/dev/null || echo unknown)"
fi
if [ -r /proc/meminfo ]; then
    grep MemTotal /proc/meminfo
elif command -v sysctl >/dev/null 2>&1; then
    mem_bytes=$(sysctl -n hw.memsize 2>/dev/null || echo 0)
    echo "MemTotal: $((mem_bytes / 1024)) kB"
fi
echo "NATS_URL: $NATS_URL"
nats --version 2>&1 | head -1
command -v nats-server >/dev/null 2>&1 && nats-server --version 2>&1 | head -1
echo

# --- monoblok --------------------------------------------------------------
"$MB_BIN" --port $PORT > /tmp/mb.log 2>&1 &
MB_PID=$!
sleep 0.3
kill -0 $MB_PID 2>/dev/null || { echo "monoblok failed to start:"; cat /tmp/mb.log; exit 1; }

echo "=== monoblok ==="
head -3 /tmp/mb.log
echo
run_sweep
kill $MB_PID 2>/dev/null; wait $MB_PID 2>/dev/null || true
MB_PID=""
sleep 0.2

# --- nats-server (optional) ------------------------------------------------
if command -v nats-server >/dev/null 2>&1; then
    nats-server --port $PORT > /tmp/ns.log 2>&1 &
    NS_PID=$!
    sleep 0.4
    if kill -0 $NS_PID 2>/dev/null; then
        echo
        echo "=== nats-server ==="
        run_sweep
    else
        echo "nats-server failed to start:"
        cat /tmp/ns.log
    fi
else
    echo
    echo "(nats-server not installed — skipping comparison)"
fi

echo
echo "done."
