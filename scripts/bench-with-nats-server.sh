#!/usr/bin/env bash
# Benchmark monoblok against nats-server using the nats CLI.
#
# The C daemon implements the core CONNECT/PING/PUB/SUB and fanout path, so this
# measures core parser/router/write behavior rather than Monoblok feature parity.
#
# Usage: ./scripts/bench-with-nats-server.sh [--io-uring|--epoll]
#
# Linux monoblok benchmark runs use libuv io_uring by default. Pass --epoll
# when you want the production-default Linux path for comparison.
set -u

usage() {
    echo "Usage: $0 [--io-uring|--epoll]"
    echo
    echo "On Linux, defaults to --io-uring for monoblok. Pass --epoll to run with libuv epoll."
}

BENCH_IO_MODE="${BENCH_IO_MODE:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --io-uring)
            BENCH_IO_MODE="io_uring"
            ;;
        --epoll|--no-io-uring)
            BENCH_IO_MODE="epoll"
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
    shift
done

HERE="$(cd "$(dirname "$0")" && pwd)"
# Binary lives next to the script on deployed boxes (release tarballs);
# fall back to cmake-out/bin when run out of a source checkout.
if [ -x "$HERE/monoblok" ]; then
    MB_BIN="$HERE/monoblok"
    ROOT="$HERE"
else
    ROOT="$(cd "$HERE/.." && pwd)"
    MB_BIN="$ROOT/build/monoblok"
fi

export NATS_URL="${NATS_URL:-nats://127.0.0.1:4222}"
PORT="${NATS_URL##*:}"
COOLDOWN_S="${BENCH_COOLDOWN_S:-1}"
MB_IO_ARGS=()
MB_IO_LABEL="platform default"
if [[ "$(uname)" == "Linux" ]]; then
    case "${BENCH_IO_MODE:-io_uring}" in
        io_uring|io-uring)
            MB_IO_ARGS=(--io-uring)
            MB_IO_LABEL="io_uring"
            ;;
        epoll)
            MB_IO_ARGS=(--no-io-uring)
            MB_IO_LABEL="epoll"
            ;;
        *)
            echo "invalid BENCH_IO_MODE: $BENCH_IO_MODE (use io_uring or epoll)"
            exit 2
            ;;
    esac
fi

command -v nats >/dev/null 2>&1 || {
    echo "missing: nats CLI (install from https://github.com/nats-io/natscli/releases)"
    exit 1
}

HAS_NATS_SERVER=false
if command -v nats-server >/dev/null 2>&1; then
    HAS_NATS_SERVER=true
fi

if [ ! -x "$MB_BIN" ]; then
    echo "building monoblok..."
    cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
    cmake --build "$ROOT/build" --target monoblok >/dev/null || exit 1
fi

cleanup() {
    [ -n "${MB_PID:-}" ] && kill "$MB_PID" 2>/dev/null && wait "$MB_PID" 2>/dev/null || true
    [ -n "${NS_PID:-}" ] && kill "$NS_PID" 2>/dev/null && wait "$NS_PID" 2>/dev/null || true
}
trap cleanup EXIT

extract_rate() {
    local kind="$1"
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
    nats --no-context bench pub --clients="$clients" --msgs="$msgs" --size="$size" solo 2>&1 \
        | extract_rate publisher
}

run_fanout() {
    local subs="$1" msgs="$2"
    local out="/tmp/monoblok-bench-sub.out"
    nats --no-context bench sub --clients="$subs" --msgs="$msgs" fan > "$out" 2>&1 &
    local sub_pid=$!
    sleep 0.8
    nats --no-context bench pub --clients=1 --msgs="$msgs" --size=64 fan > /dev/null 2>&1
    wait "$sub_pid" 2>/dev/null || true
    extract_rate subscriber < "$out"
}

parse_num() {
    echo "${1:-0}" | tr -d ','
}

fmt_num() {
    printf "%'d" "$1" 2>/dev/null || echo "$1"
}

calc_delta() {
    local a="$1" b="$2"
    if [ "$b" -eq 0 ]; then
        echo "-"
        return
    fi
    awk -v a="$a" -v b="$b" 'BEGIN {
        d = (a - b) / b * 100
        if (d >= 0) printf "+%.0f%%", d
        else printf "%.0f%%", d
    }'
}

print_row() {
    local label="$1" mb="$2" ns="$3"
    local mb_n
    mb_n=$(parse_num "$mb")
    if [ -n "$ns" ]; then
        local ns_n delta
        ns_n=$(parse_num "$ns")
        delta=$(calc_delta "$mb_n" "$ns_n")
        printf "%-24s %14s %14s %8s\n" "$label" "$(fmt_num "$mb_n")" "$(fmt_num "$ns_n")" "$delta"
    else
        printf "%-24s %14s\n" "$label" "$(fmt_num "$mb_n")"
    fi
}

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
echo "$("$MB_BIN" -V 2>&1 | head -1 || true)"
echo "nats cli $(nats --version 2>&1 | head -1)"
$HAS_NATS_SERVER && echo "$(nats-server --version 2>&1 | head -1)"
echo "monoblok libuv mode: $MB_IO_LABEL"
echo

"$MB_BIN" ${MB_IO_ARGS[@]+"${MB_IO_ARGS[@]}"} --port "$PORT" > /tmp/monoblok-bench.log 2>&1 &
MB_PID=$!
sleep 0.3
kill -0 "$MB_PID" 2>/dev/null || {
    echo "monoblok failed to start:"
    cat /tmp/monoblok-bench.log
    exit 1
}

echo "Running monoblok benchmarks..."
MB_1=$(run_pub 1 1000000 64);    sleep "$COOLDOWN_S"
MB_2=$(run_pub 2 1000000 64);    sleep "$COOLDOWN_S"
MB_3=$(run_pub 8 250000 128);    sleep "$COOLDOWN_S"
MB_4=$(run_fanout 1 500000);     sleep "$COOLDOWN_S"
MB_5=$(run_fanout 10 100000);    sleep "$COOLDOWN_S"
MB_6=$(run_fanout 50 50000)
kill "$MB_PID" 2>/dev/null; wait "$MB_PID" 2>/dev/null || true
MB_PID=""
sleep "$COOLDOWN_S"

NS_1="" NS_2="" NS_3="" NS_4="" NS_5="" NS_6=""
if $HAS_NATS_SERVER; then
    nats-server --port "$PORT" > /tmp/monoblok-nats-server.log 2>&1 &
    NS_PID=$!
    sleep 0.4
    if kill -0 "$NS_PID" 2>/dev/null; then
        echo "Running nats-server benchmarks..."
        NS_1=$(run_pub 1 1000000 64);    sleep "$COOLDOWN_S"
        NS_2=$(run_pub 2 1000000 64);    sleep "$COOLDOWN_S"
        NS_3=$(run_pub 8 250000 128);    sleep "$COOLDOWN_S"
        NS_4=$(run_fanout 1 500000);     sleep "$COOLDOWN_S"
        NS_5=$(run_fanout 10 100000);    sleep "$COOLDOWN_S"
        NS_6=$(run_fanout 50 50000)
        kill "$NS_PID" 2>/dev/null; wait "$NS_PID" 2>/dev/null || true
        NS_PID=""
    else
        echo "nats-server failed to start:"
        cat /tmp/monoblok-nats-server.log
    fi
fi

echo
echo "=== Results (msgs/sec) ==="
echo
if [ -n "$NS_1" ]; then
    printf "%-24s %14s %14s %8s\n" "Workload" "monoblok" "nats-server" "delta"
    printf "%-24s %14s %14s %8s\n" "--------" "----------" "-----------" "-----"
    print_row "1 pub x 1M x 64B" "$MB_1" "$NS_1"
    print_row "2 pub x 1M x 64B" "$MB_2" "$NS_2"
    print_row "8 pub x 250k x 128B" "$MB_3" "$NS_3"
    print_row "1 pub -> 1 sub x 500k" "$MB_4" "$NS_4"
    print_row "1 pub -> 10 subs x 100k" "$MB_5" "$NS_5"
    print_row "1 pub -> 50 subs x 50k" "$MB_6" "$NS_6"
else
    printf "%-24s %14s\n" "Workload" "monoblok"
    printf "%-24s %14s\n" "--------" "----------"
    print_row "1 pub x 1M x 64B" "$MB_1" ""
    print_row "2 pub x 1M x 64B" "$MB_2" ""
    print_row "8 pub x 250k x 128B" "$MB_3" ""
    print_row "1 pub -> 1 sub x 500k" "$MB_4" ""
    print_row "1 pub -> 10 subs x 100k" "$MB_5" ""
    print_row "1 pub -> 50 subs x 50k" "$MB_6" ""
fi
