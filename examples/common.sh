: "${PORT:=14222}"
URL="nats://127.0.0.1:$PORT"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/zig-out/bin/monoblok"
NAME="$(basename "${0%.sh}")"
TMP="/tmp/monoblok-$NAME"

if [ ! -x "$BIN" ]; then
    echo "building monoblok..."
    (cd "$ROOT" && zig build) || exit 1
fi

if ! command -v nats >/dev/null; then
    echo "the nats CLI is required (https://github.com/nats-io/natscli)" >&2
    exit 1
fi

rm -rf "$TMP"
mkdir -p "$TMP"

_TITLE="$NAME.sh"
echo "$_TITLE"
printf '%*s\n' "${#_TITLE}" '' | tr ' ' '='
echo
echo "Please wait ..."
echo

EXTRA_PIDS=()

cleanup() {
    if [ -n "${DAEMON_PID:-}" ]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    if [ ${#EXTRA_PIDS[@]} -gt 0 ]; then
        kill "${EXTRA_PIDS[@]}" 2>/dev/null || true
        wait "${EXTRA_PIDS[@]}" 2>/dev/null || true
    fi
    local pids
    pids=$(jobs -p)
    if [ -n "$pids" ]; then
        kill $pids 2>/dev/null || true
        wait $pids 2>/dev/null || true
    fi
}
trap cleanup EXIT

start_daemon() {
    "$BIN" --port "$PORT" --patchbay "$ROOT/$1" >"$TMP/daemon.log" 2>&1 &
    DAEMON_PID=$!
    sleep 0.4
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "daemon failed to start:"
        cat "$TMP/daemon.log"
        exit 1
    fi
}

subscribe() {
    nats --no-context -s "$URL" sub -d "$1" 2>&1 | awk -v OFS="\t" '
        /^\[#[0-9]+\] @ / {
            line = $0
            sub(/^\[#[0-9]+\] @ /, "", line)
            sub(/ Received on "/, "\t", line)
            sub(/"$/, "", line)
            if ((getline payload) > 0) { print line, payload; fflush() }
            next
        }
        /^[[:space:]]*$/ { next }
        /^[0-9]+:[0-9]+:[0-9]+ / { next }
        { print $0; fflush() }
    ' >"$TMP/$2" &
}

pub() {
    nats --no-context -s "$URL" pub "$1" "$2" 2>&1 | awk -v payload="$2" '
        /^[0-9]+:[0-9]+:[0-9]+ Published / {
            print ">>> " $0 "  " payload
        }
    ' >>"$TMP/_pubs.log"
}

show() {
    local label="$1" file="$2" max="${3:-}"
    echo
    echo "===== $label ($file) ====="
    if [ ! -s "$TMP/$file" ]; then
        echo "(empty)"
        echo
        return
    fi
    local src="$TMP/$file"
    if [ -n "$max" ]; then
        local total
        total=$(wc -l <"$src" | tr -d ' ')
        if [ "$total" -gt "$max" ]; then
            echo "(showing last $max of $total lines, full file: $src)"
        fi
        tail -n "$max" "$src" | python3 "$ROOT/examples/_tabulate.py"
    else
        python3 "$ROOT/examples/_tabulate.py" <"$src"
    fi
    echo
}

settle() {
    sleep "${1:-0.4}"
}

note() {
    echo
    for line in "$@"; do
        echo "  * $line"
    done
}
