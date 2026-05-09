#!/usr/bin/env bash
# CPU-utilization smoke for mixer mode. Spawns mixer + 2 workers, drives
# sustained PUB traffic split across both shards, samples per-PID %CPU
# while the load is running. Workers should each show meaningful %CPU
# (proving they execute on separate cores). The mixer process should
# also show load (it parses every frame).
#
# Pass criterion: each worker averages >= MIN_WORKER_CPU (default 20%)
# during the load window. Adjust via env if your box is slow.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PORT:-14334}"
BIN="$ROOT/zig-out/bin/monoblok"
DURATION="${DURATION:-6}"           # seconds of load
SAMPLE_INTERVAL="${SAMPLE_INTERVAL:-1}"
MIN_WORKER_CPU="${MIN_WORKER_CPU:-20}"
CONNS="${CONNS:-1}"                 # parallel publisher TCP connections
TMP="$(mktemp -d /tmp/monoblok-mixer-cpu.XXXXXX)"
MIXER_EDN="$TMP/mixer.edn"
T_EDN="$TMP/t.edn"
DEFAULT_EDN="$TMP/default.edn"
MIXER_LOG="$TMP/mixer.log"
T_LOG="$TMP/t.log"
DEFAULT_LOG="$TMP/default.log"
SAMPLES="$TMP/samples.txt"

if [ ! -x "$BIN" ]; then
    echo "building (release=safe)..."
    (cd "$ROOT" && zig build --release=safe) || exit 1
fi

cleanup() {
    if [ -n "${PUB_PID:-}" ]; then kill "$PUB_PID" 2>/dev/null || true; fi
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

# Quiet rules so workers spend cycles on parsing/fan-out, not stderr writes.
echo '(on ">" payload)' > "$T_EDN"
echo '(on ">" payload)' > "$DEFAULT_EDN"

cat > "$MIXER_EDN" <<EOF
(mixer
  :listen "tcp://0.0.0.0:$PORT"
  :workers
    ((:shard "T" :patchbay "$T_EDN" :log "$T_LOG")
     (:shard "*" :patchbay "$DEFAULT_EDN" :log "$DEFAULT_LOG")))
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

# Parse worker PIDs out of the mixer log. Format:
#   "mixer spawned worker shard=T pid=12345 fd=..."
T_PID="$(grep -oE 'shard=T pid=[0-9]+' "$MIXER_LOG" | head -1 | grep -oE '[0-9]+$' || true)"
DEFAULT_PID="$(grep -oE 'shard=\* pid=[0-9]+' "$MIXER_LOG" | head -1 | grep -oE '[0-9]+$' || true)"

if [ -z "$T_PID" ] || [ -z "$DEFAULT_PID" ]; then
    echo "FAIL: could not parse worker PIDs from mixer log"
    cat "$MIXER_LOG"
    exit 1
fi

echo "mixer pid=$MIXER_PID  T worker pid=$T_PID  default worker pid=$DEFAULT_PID"

# Sustained publisher: K parallel TCP connections, each sending half its
# traffic to T.* and half to ORDERS.* so both workers get hit. K is
# controlled by $CONNS so we can compare 1 fat connection vs N thinner ones.
echo "publisher: $CONNS connection(s) for ${DURATION}s..."
python3 - "$PORT" "$DURATION" "$CONNS" <<'PY' &
import socket, sys, time, threading
port = int(sys.argv[1])
dur = float(sys.argv[2])
k = int(sys.argv[3])
payload = b"x" * 64
frame_t = b"PUB T.AAPL.p 64\r\n" + payload + b"\r\n"
frame_o = b"PUB ORDERS.42 64\r\n" + payload + b"\r\n"
buf = (frame_t + frame_o) * 64  # ~8 KiB per write
totals = [0] * k
def run(i):
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(b"CONNECT {}\r\n")
    deadline = time.monotonic() + dur
    n = 0
    while time.monotonic() < deadline:
        s.sendall(buf)
        n += 128
    totals[i] = n
    s.close()
threads = [threading.Thread(target=run, args=(i,)) for i in range(k)]
for t in threads: t.start()
for t in threads: t.join()
print(f"published {sum(totals)} msgs across {k} conns", file=sys.stderr)
PY
PUB_PID=$!

# Sample %CPU while load is running. Use top in batch mode; format
# differs between macOS (top -l) and linux (top -b -n).
> "$SAMPLES"
samples=0
end=$(( $(date +%s) + DURATION ))
case "$(uname -s)" in
    Darwin)
        # top -l N: N samples, interval defaults to 1s. -stats limits columns.
        # Run it in a single invocation matching the load duration.
        N=$(( DURATION / SAMPLE_INTERVAL ))
        [ "$N" -lt 2 ] && N=2
        top -l "$N" -s "$SAMPLE_INTERVAL" -pid "$MIXER_PID" -pid "$T_PID" -pid "$DEFAULT_PID" -stats pid,cpu,command \
            | awk -v m="$MIXER_PID" -v t="$T_PID" -v d="$DEFAULT_PID" '
                $1 == m || $1 == t || $1 == d { print $1, $2 }
              ' > "$SAMPLES"
        ;;
    Linux)
        while [ "$(date +%s)" -lt "$end" ]; do
            top -b -n 1 -p "$MIXER_PID,$T_PID,$DEFAULT_PID" 2>/dev/null \
                | awk -v m="$MIXER_PID" -v t="$T_PID" -v d="$DEFAULT_PID" '
                    $1 == m || $1 == t || $1 == d { print $1, $9 }
                  ' >> "$SAMPLES"
            sleep "$SAMPLE_INTERVAL"
        done
        ;;
    *)
        echo "FAIL: unsupported OS for CPU sampling"
        exit 1
        ;;
esac

wait "$PUB_PID" 2>/dev/null || true

# Average %CPU per pid across samples. top's first sample on macOS is
# since-process-start (often near zero), so we drop the first row per pid.
avg() {
    local pid="$1"
    awk -v p="$pid" '
        $1 == p {
            n++
            if (n > 1) { sum += $2; k++ }
        }
        END { if (k > 0) printf "%.1f", sum / k; else print "0" }
    ' "$SAMPLES"
}

MIXER_CPU=$(avg "$MIXER_PID")
T_CPU=$(avg "$T_PID")
DEFAULT_CPU=$(avg "$DEFAULT_PID")

echo
echo "avg %CPU over ${DURATION}s load:"
printf "  mixer    (pid %s): %s%%\n" "$MIXER_PID" "$MIXER_CPU"
printf "  T worker (pid %s): %s%%\n" "$T_PID" "$T_CPU"
printf "  * worker (pid %s): %s%%\n" "$DEFAULT_PID" "$DEFAULT_CPU"

fail=0
awk -v v="$T_CPU" -v min="$MIN_WORKER_CPU" 'BEGIN{ exit !(v+0 >= min+0) }' \
    || { echo "FAIL: T worker CPU below ${MIN_WORKER_CPU}%"; fail=1; }
awk -v v="$DEFAULT_CPU" -v min="$MIN_WORKER_CPU" 'BEGIN{ exit !(v+0 >= min+0) }' \
    || { echo "FAIL: default worker CPU below ${MIN_WORKER_CPU}%"; fail=1; }

[ "$fail" -eq 0 ] && echo "ok: both workers showed sustained CPU (>= ${MIN_WORKER_CPU}%)"
exit $fail
