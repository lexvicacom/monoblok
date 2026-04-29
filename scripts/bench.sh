#!/usr/bin/env bash
# Bench monoblok at three patchbay sizes against the same workload:
# no patchbay, one rule, fifty rules. Used to characterize the per-PUB
# cost of rule loading and dispatch.
#
# For a head-to-head comparison against upstream nats-server, see
# scripts/bench-with-nats-server.sh.
#
# Honours NATS_URL (same env var the nats CLI reads) for the address;
# defaults to nats://127.0.0.1:4222.
#
# Requirements:
#   - nats CLI (https://github.com/nats-io/natscli/releases)
#
# Usage: ./scripts/bench.sh
#
# NOTE: If numbers seem low, ensure you're running a release build:
#   zig build --release=safe   (recommended, what release artifacts ship)
#   zig build --release=fast   (slightly faster, no safety checks)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
# Binary lives next to the script on deployed boxes (release tarballs);
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

if [ ! -x "$MB_BIN" ]; then
    echo "building..."
    (cd "$ROOT" && zig build --release=safe) || exit 1
fi

cleanup() {
    [ -n "${MB_PID:-}" ] && kill "$MB_PID" 2>/dev/null && wait "$MB_PID" 2>/dev/null || true
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
    nats --no-context bench pub --clients=$clients --msgs=$msgs --size=$size solo 2>&1 \
        | extract_rate publisher
}

run_fanout() {
    local subs="$1" msgs="$2"
    nats --no-context bench sub --clients=$subs --msgs=$msgs fan > /tmp/bench_sub.out 2>&1 &
    local sub_pid=$!
    sleep 0.8
    nats --no-context bench pub --clients=1 --msgs=$msgs --size=64 fan > /dev/null 2>&1
    wait $sub_pid 2>/dev/null || true
    extract_rate subscriber < /tmp/bench_sub.out
}

# Convert "1,234,567" to 1234567
parse_num() {
    echo "${1:-0}" | tr -d ','
}

# Format number with commas
fmt_num() {
    printf "%'d" "$1" 2>/dev/null || echo "$1"
}

# Calculate percentage difference: (a - b) / b * 100
calc_delta() {
    local a="$1" b="$2"
    if [ "$b" -eq 0 ]; then
        echo "—"
        return
    fi
    awk -v a="$a" -v b="$b" 'BEGIN {
        d = (a - b) / b * 100
        if (d >= 0) printf "+%.0f%%", d
        else printf "%.0f%%", d
    }'
}

# --- Host info ----------------------------------------------------------------
echo "=== Host ==="
echo
if [[ "$(uname)" == "Darwin" ]]; then
    sw_vers 2>/dev/null | awk -F: '/ProductName|ProductVersion/ {gsub(/^[ \t]+/, "", $2); printf "%s ", $2} END {print ""}'
    echo "$(uname -m) · $(sysctl -n hw.ncpu) cores · $(($(sysctl -n hw.memsize) / 1024 / 1024 / 1024)) GB"
else
    uname -sr
    cores=$(nproc 2>/dev/null || echo "?")
    mem=$(awk '/MemTotal/ {printf "%.0f", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo "?")
    echo "$(uname -m) · ${cores} cores · ${mem} GB"
fi
echo
echo "$("$MB_BIN" --version)"
echo "nats cli $(nats --version 2>&1 | head -1)"
echo

# --- monoblok -----------------------------------------------------------------
"$MB_BIN" --port $PORT > /tmp/mb.log 2>&1 &
MB_PID=$!
sleep 0.3
kill -0 $MB_PID 2>/dev/null || { echo "monoblok failed to start:"; cat /tmp/mb.log; exit 1; }

# Cooldown between rows. Back-to-back 500k+ msg runs throttle laptop CPUs;
# 5s is enough to let the cores cool back to baseline. Tune via
# BENCH_COOLDOWN_S if you want a different value.
COOLDOWN_S="${BENCH_COOLDOWN_S:-5}"

echo "Running monoblok benchmarks..."
MB_1=$(run_pub 1 1000000 64);    sleep "$COOLDOWN_S"
MB_2=$(run_pub 2 500000 64);     sleep "$COOLDOWN_S"
MB_3=$(run_pub 8 200000 128);    sleep "$COOLDOWN_S"
MB_4=$(run_fanout 1 500000);     sleep "$COOLDOWN_S"
MB_5=$(run_fanout 10 200000);    sleep "$COOLDOWN_S"
MB_6=$(run_fanout 50 50000)
kill $MB_PID 2>/dev/null; wait $MB_PID 2>/dev/null || true
MB_PID=""
sleep "$COOLDOWN_S"

# --- monoblok + runtime patchbay (one rule) -----------------------------------
# One rule, gate false on bench payloads. Measures the per-PUB cost of running
# a single matching rule's body without extra fan-out work.
PATCHBAY_EDN="$ROOT/examples/bench-onerule.edn"
MB_P_1="" MB_P_2="" MB_P_3="" MB_P_4="" MB_P_5="" MB_P_6=""
if [ -f "$PATCHBAY_EDN" ]; then
    "$MB_BIN" --port $PORT --patchbay "$PATCHBAY_EDN" > /tmp/mb-patchbay.log 2>&1 &
    MB_PID=$!
    sleep 0.3
    if kill -0 $MB_PID 2>/dev/null; then
        echo "Running monoblok+patchbay (1 rule) benchmarks..."
        MB_P_1=$(run_pub 1 1000000 64);    sleep "$COOLDOWN_S"
        MB_P_2=$(run_pub 2 500000 64);     sleep "$COOLDOWN_S"
        MB_P_3=$(run_pub 8 200000 128);    sleep "$COOLDOWN_S"
        MB_P_4=$(run_fanout 1 500000);     sleep "$COOLDOWN_S"
        MB_P_5=$(run_fanout 10 200000);    sleep "$COOLDOWN_S"
        MB_P_6=$(run_fanout 50 50000)
        kill $MB_PID 2>/dev/null; wait $MB_PID 2>/dev/null || true
        MB_PID=""
        sleep "$COOLDOWN_S"
    else
        echo "monoblok+patchbay (1 rule) failed to start:"
        cat /tmp/mb-patchbay.log
    fi
fi

# --- monoblok + runtime patchbay (50 rules) -----------------------------------
# 50 rules, only the last filter matches "solo". Measures the per-PUB cost
# of the linear filter scan + one matching body.
PATCHBAY_50_EDN="$ROOT/examples/bench-50rules.edn"
MB_50_1="" MB_50_2="" MB_50_3="" MB_50_4="" MB_50_5="" MB_50_6=""
if [ -f "$PATCHBAY_50_EDN" ]; then
    "$MB_BIN" --port $PORT --patchbay "$PATCHBAY_50_EDN" > /tmp/mb-patchbay50.log 2>&1 &
    MB_PID=$!
    sleep 0.3
    if kill -0 $MB_PID 2>/dev/null; then
        echo "Running monoblok+patchbay (50 rules) benchmarks..."
        MB_50_1=$(run_pub 1 1000000 64);    sleep "$COOLDOWN_S"
        MB_50_2=$(run_pub 2 500000 64);     sleep "$COOLDOWN_S"
        MB_50_3=$(run_pub 8 200000 128);    sleep "$COOLDOWN_S"
        MB_50_4=$(run_fanout 1 500000);     sleep "$COOLDOWN_S"
        MB_50_5=$(run_fanout 10 200000);    sleep "$COOLDOWN_S"
        MB_50_6=$(run_fanout 50 50000)
        kill $MB_PID 2>/dev/null; wait $MB_PID 2>/dev/null || true
        MB_PID=""
        sleep "$COOLDOWN_S"
    else
        echo "monoblok+patchbay (50 rules) failed to start:"
        cat /tmp/mb-patchbay50.log
    fi
fi

# --- Results table ------------------------------------------------------------
echo
echo "=== Results (msgs/sec) ==="
echo

# Δ vs no-patchbay baseline. Negative means slower than baseline.
print_row() {
    local label="$1" base="$2" one="$3" fifty="$4"
    local b=$(parse_num "$base") o=$(parse_num "$one") f=$(parse_num "$fifty")
    local d1=$(calc_delta "$o" "$b")
    local d50=$(calc_delta "$f" "$b")
    printf "%-22s %12s %12s %8s %12s %8s\n" \
        "$label" "$(fmt_num $b)" "$(fmt_num $o)" "$d1" "$(fmt_num $f)" "$d50"
}

printf "%-22s %12s %12s %8s %12s %8s\n" "Workload" "no patchbay" "1 rule" "Δ" "50 rules" "Δ"
printf "%-22s %12s %12s %8s %12s %8s\n" "--------" "-----------" "------" "---" "--------" "---"
print_row "1 pub × 1M × 64B"    "$MB_1" "$MB_P_1" "$MB_50_1"
print_row "2 pub × 500k × 64B"  "$MB_2" "$MB_P_2" "$MB_50_2"
print_row "8 pub × 200k × 128B" "$MB_3" "$MB_P_3" "$MB_50_3"
print_row "1 pub → 1 sub"       "$MB_4" "$MB_P_4" "$MB_50_4"
print_row "1 pub → 10 subs"     "$MB_5" "$MB_P_5" "$MB_50_5"
print_row "1 pub → 50 subs"     "$MB_6" "$MB_P_6" "$MB_50_6"

echo
echo "NOTE: If numbers seem low, ensure a release build (zig build --release=safe)"
