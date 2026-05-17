#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

if ! command -v nats-server >/dev/null; then
    echo "nats-server is required (https://github.com/nats-io/nats-server)" >&2
    exit 1
fi

REMOTE_PORT=14889
REMOTE_URL="nats://127.0.0.1:$REMOTE_PORT"

nats-server -p $REMOTE_PORT >"$TMP/remote.log" 2>&1 &
REMOTE_PID=$!
EXTRA_PIDS+=("$REMOTE_PID")
sleep 0.4
if ! kill -0 "$REMOTE_PID" 2>/dev/null; then
    echo "nats-server failed to start:"
    cat "$TMP/remote.log"
    exit 1
fi

start_daemon examples/import.edn

nats --no-context -s "$REMOTE_URL" sub -d 'raw.>' 2>&1 | awk -v OFS="\t" '
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
' >"$TMP/remote-raw.txt" &

nats --no-context -s "$REMOTE_URL" sub -d 'clean.>' 2>&1 | awk -v OFS="\t" '
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
' >"$TMP/remote-clean.txt" &

subscribe 'raw.>' local-raw.txt
subscribe 'clean.>' local-clean.txt
settle 0.4

remote_pub() {
    nats --no-context -s "$REMOTE_URL" pub "$1" "$2" 2>&1 | awk -v payload="$2" '
        /^[0-9]+:[0-9]+:[0-9]+ Published / {
            print ">>> remote " $0 "  " payload
        }
    ' >>"$TMP/_pubs.log"
}

for v in 42.01 42.04 42.08 42.12 43.00 43.00 43.01; do
    remote_pub raw.temp "$v"
done

settle 0.6

note \
    "Seven raw.temp samples were published to the standalone nats-server on port $REMOTE_PORT." \
    "monoblok imports raw.>, rounds to 1dp, squelches duplicates, and publishes clean.temp locally." \
    "The clean.> output is exported back to the standalone nats-server." \
    "The local raw.> subscription stays empty because imported raw messages are private patchbay ingress."
show "remote publishes"              _pubs.log
show "remote raw.> source stream"    remote-raw.txt
show "remote clean.> exported stream" remote-clean.txt
show "local raw.> (private ingress)" local-raw.txt
show "local clean.> output"          local-clean.txt
