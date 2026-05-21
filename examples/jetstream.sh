#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

if ! command -v nats-server >/dev/null; then
    echo "nats-server is required (https://github.com/nats-io/nats-server)" >&2
    exit 1
fi

if [ -n "${JS_URL:-}" ] && [ -z "${JS_PORT:-}" ]; then
    JS_PORT="$(python3 - "$JS_URL" <<'PY'
import sys
from urllib.parse import urlparse

url = sys.argv[1]
parsed = urlparse(url if "://" in url else f"nats://{url}")
print(parsed.port or 4222)
PY
)"
fi
if [ -n "${BRIDGE_URL:-}" ] && [ -z "${BRIDGE_PORT:-}" ]; then
    BRIDGE_PORT="$(python3 - "$BRIDGE_URL" <<'PY'
import sys
from urllib.parse import urlparse

url = sys.argv[1]
parsed = urlparse(url if "://" in url else f"nats://{url}")
print(parsed.port or 4222)
PY
)"
fi
JS_PORT="${JS_PORT:-15889}"
JS_URL="${JS_URL:-nats://127.0.0.1:$JS_PORT}"
export JS_URL
BRIDGE_PORT="${BRIDGE_PORT:-15890}"
BRIDGE_URL="${BRIDGE_URL:-nats://127.0.0.1:$BRIDGE_PORT}"
export BRIDGE_URL
COUNT="${COUNT:-1000}"
if [ -z "${PAUSE_EVERY:-}" ]; then
    if [ "$COUNT" -gt 1 ]; then
        PAUSE_EVERY=$((COUNT / 2))
        if [ "$PAUSE_EVERY" -lt 1 ]; then
            PAUSE_EVERY=1
        fi
    else
        PAUSE_EVERY=0
    fi
fi
PAUSE_MS="${PAUSE_MS:-1100}"

nats-server -js -sd "$TMP/js-store" -p "$JS_PORT" >"$TMP/js-server.log" 2>&1 &
JS_PID=$!
EXTRA_PIDS+=("$JS_PID")
nats-server -p "$BRIDGE_PORT" >"$TMP/bridge-server.log" 2>&1 &
BRIDGE_PID=$!
EXTRA_PIDS+=("$BRIDGE_PID")

for _ in $(seq 1 50); do
    kill -0 "$JS_PID" 2>/dev/null || break
    grep -q 'Server is ready' "$TMP/js-server.log" && break
    sleep 0.1
done
if ! kill -0 "$JS_PID" 2>/dev/null; then
    echo "JetStream nats-server failed to start:"
    cat "$TMP/js-server.log"
    exit 1
fi
if ! grep -q 'Server is ready' "$TMP/js-server.log"; then
    echo "JetStream nats-server did not become ready:"
    cat "$TMP/js-server.log"
    exit 1
fi
for _ in $(seq 1 50); do
    kill -0 "$BRIDGE_PID" 2>/dev/null || break
    grep -q 'Server is ready' "$TMP/bridge-server.log" && break
    sleep 0.1
done
if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
    echo "bridge nats-server failed to start:"
    cat "$TMP/bridge-server.log"
    exit 1
fi
if ! grep -q 'Server is ready' "$TMP/bridge-server.log"; then
    echo "bridge nats-server did not become ready:"
    cat "$TMP/bridge-server.log"
    exit 1
fi

JS_URL="$JS_URL" COUNT="$COUNT" PAUSE_EVERY="$PAUSE_EVERY" PAUSE_MS="$PAUSE_MS" "$ROOT/examples/jetstream-populate.sh" >"$TMP/populate.log"

bridge_subscribe() {
    monitor "$BRIDGE_URL" "$1" "$2"
}

bridge_subscribe 'js.>' bridge-js.txt
bridge_subscribe 'js.replay.last-temp' bridge-headers.txt
bridge_subscribe 'js.sensors.temp' bridge-raw.txt
settle 0.4

start_daemon examples/jetstream.yml
for _ in $(seq 1 80); do
    grep -q 'jetstream import: connected' "$TMP/daemon.log" && break
    sleep 0.1
done
if ! grep -q 'jetstream import: connected' "$TMP/daemon.log"; then
    echo "monoblok did not finish JetStream catch-up:"
    cat "$TMP/daemon.log"
    exit 1
fi

subscribe '$LVC.js.>' lvc.txt
subscribe 'js.live.temp' live.txt
subscribe 'js.sensors.temp' raw.txt
settle 0.4

nats --no-context -s "$JS_URL" pub js.sensors.temp 25.50 >"$TMP/live-pub.log" 2>&1
settle 0.8

note \
    "Started a JetStream nats-server on port $JS_PORT." \
    "Started a plain NATS bridge target on port $BRIDGE_PORT and exported js.> outputs to it." \
    "Populated stream SENSORS with $COUNT historical js.sensors.temp events before monoblok started." \
    "Population pauses every $PAUSE_EVERY events for ${PAUSE_MS}ms so replay catch-up closes a time bar from JetStream timestamps." \
    "monoblok replayed the stream before opening its listener, so late LVC subscribers see warm replay-derived state." \
    "The bridge subscription was active during catch-up, so replay-derived exports appear there even while monoblok's listener was closed." \
    "The export config enables origin-header and replay-header, so bridged replay outputs include x-monoblok provenance, x-monoblok-replay, and x-monoblok-assumed-ts; live outputs omit replay headers." \
    "A final live event was published after startup and appears on js.live.temp; count! therefore reaches COUNT + 1." \
    "The raw js.sensors.temp source remains private patchbay ingress."

show "populate"                       populate.log
show "monoblok JetStream startup"      daemon.log 20
show "late LVC view after replay + live" lvc.txt 20
show "bridge target js.> exports"        bridge-js.txt 20
show "bridge target replay headers"      bridge-headers.txt 20
show "bridge target raw source"         bridge-raw.txt 20
show "live output after catch-up"       live.txt 20
show "local raw source subscription"   raw.txt 20
