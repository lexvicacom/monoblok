#!/usr/bin/env bash
# Bench monoblok and nats-server on the current host. Same workload is
# driven against both, one at a time (not concurrently).
#
# Monoblok runs with --no-lvc here so the comparison measures core routing
# throughput on both sides. nats-server has no last-value cache, so leaving
# monoblok's LVC on would be measuring an extra feature, not the routing
# path itself. For LVC-on numbers see bench.sh.
#
# Honours NATS_URL (same env var the nats CLI reads) for the address;
# defaults to nats://127.0.0.1:4222. Both servers bind to the same port
# but are started in sequence.
#
# Requirements:
#   - nats CLI     (https://github.com/nats-io/natscli/releases)
#   - nats-server  (optional; comparison section skipped if absent)
#
# Usage: ./scripts/bench-with-nats-server.sh
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

echo $MB_BIN
export NATS_URL="${NATS_URL:-nats://127.0.0.1:4222}"
# Extract host:port → port for the server --port flag.
PORT="${NATS_URL##*:}"

command -v nats >/dev/null 2>&1 || { echo "missing: nats CLI (install from https://github.com/nats-io/natscli/releases)"; exit 1; }
HAS_NATS_SERVER=false
if command -v nats-server >/dev/null 2>&1; then
    HAS_NATS_SERVER=true
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
$HAS_NATS_SERVER && echo "$(nats-server --version 2>&1 | head -1)"
echo

# --- monoblok -----------------------------------------------------------------
# --no-lvc keeps this comparison apples-to-apples with nats-server, which has
# no last-value cache. Monoblok's default is LVC-on (every PUB pays one
# hashmap upsert + payload copy); running with it on here would measure
# "monoblok plus a feature nats-server doesn't have" rather than core
# routing throughput. For real-world numbers with LVC on, see bench.sh.
"$MB_BIN" --port $PORT --no-lvc > /tmp/mb.log 2>&1 &
MB_PID=$!
sleep 0.3
kill -0 $MB_PID 2>/dev/null || { echo "monoblok failed to start:"; cat /tmp/mb.log; exit 1; }

# Cooldown between rows. Back-to-back high-rate runs throttle laptop CPUs
# (we saw 6.4M -> 3.3M monotonic decay across 6 consecutive rows on an MBA
# on battery). 5s is enough to let the cores cool back to baseline; tune
# via BENCH_COOLDOWN_S if you want a different value.
#
# Message counts are sized to keep each timed run >= ~1s at the throughput
# range we're measuring (~2-6M msgs/sec). Smaller counts finish too fast
# for the nats CLI's measurement window to settle and produce noisy rows.
COOLDOWN_S="${BENCH_COOLDOWN_S:-5}"

echo "Running monoblok benchmarks..."
MB_1=$(run_pub 1 5000000 64);    sleep "$COOLDOWN_S"
MB_2=$(run_pub 2 2000000 64);    sleep "$COOLDOWN_S"
MB_3=$(run_pub 8 1000000 128);   sleep "$COOLDOWN_S"
MB_4=$(run_fanout 1 2000000);    sleep "$COOLDOWN_S"
MB_5=$(run_fanout 10 500000);    sleep "$COOLDOWN_S"
MB_6=$(run_fanout 50 200000)
kill $MB_PID 2>/dev/null; wait $MB_PID 2>/dev/null || true
MB_PID=""
sleep "$COOLDOWN_S"

# --- nats-server (optional) ---------------------------------------------------
NS_1="" NS_2="" NS_3="" NS_4="" NS_5="" NS_6=""
if $HAS_NATS_SERVER; then
    nats-server --port $PORT > /tmp/ns.log 2>&1 &
    NS_PID=$!
    sleep 0.4
    if kill -0 $NS_PID 2>/dev/null; then
        echo "Running nats-server benchmarks..."
        NS_1=$(run_pub 1 5000000 64);    sleep "$COOLDOWN_S"
        NS_2=$(run_pub 2 2000000 64);    sleep "$COOLDOWN_S"
        NS_3=$(run_pub 8 1000000 128);   sleep "$COOLDOWN_S"
        NS_4=$(run_fanout 1 2000000);    sleep "$COOLDOWN_S"
        NS_5=$(run_fanout 10 500000);    sleep "$COOLDOWN_S"
        NS_6=$(run_fanout 50 200000)
        kill $NS_PID 2>/dev/null; wait $NS_PID 2>/dev/null || true
        NS_PID=""
    else
        echo "nats-server failed to start:"
        cat /tmp/ns.log
    fi
fi

# --- Results table ------------------------------------------------------------
echo
echo "=== Results (msgs/sec) ==="
echo

print_row() {
    local label="$1" mb="$2" ns="$3"
    local mb_n=$(parse_num "$mb")
    if [ -n "$ns" ]; then
        local ns_n=$(parse_num "$ns")
        local delta=$(calc_delta "$mb_n" "$ns_n")
        printf "%-22s %14s %14s %8s\n" "$label" "$(fmt_num $mb_n)" "$(fmt_num $ns_n)" "$delta"
    else
        printf "%-22s %14s\n" "$label" "$(fmt_num $mb_n)"
    fi
}

if [ -n "$NS_1" ]; then
    # With comparison
    printf "%-22s %14s %14s %8s\n" "Workload" "monoblok" "nats-server" "Δ"
    printf "%-22s %14s %14s %8s\n" "--------" "--------" "-----------" "---"
    print_row "1 pub × 500k × 64B"  "$MB_1" "$NS_1"
    print_row "2 pub × 10k × 64B"   "$MB_2" "$NS_2"
    print_row "8 pub × 50k × 128B"  "$MB_3" "$NS_3"
    print_row "1 pub → 1 sub"       "$MB_4" "$NS_4"
    print_row "1 pub → 10 subs"     "$MB_5" "$NS_5"
    print_row "1 pub → 50 subs"     "$MB_6" "$NS_6"
else
    # monoblok only
    printf "%-22s %14s\n" "Workload" "monoblok"
    printf "%-22s %14s\n" "--------" "--------"
    print_row "1 pub × 500k × 64B"  "$MB_1" ""
    print_row "2 pub × 10k × 64B"   "$MB_2" ""
    print_row "8 pub × 50k × 128B"  "$MB_3" ""
    print_row "1 pub → 1 sub"       "$MB_4" ""
    print_row "1 pub → 10 subs"     "$MB_5" ""
    print_row "1 pub → 50 subs"     "$MB_6" ""
    echo
    echo "(nats-server not installed — skipping comparison)"
fi

echo
echo "NOTE: If numbers seem low, ensure a release build (zig build --release=safe)"
