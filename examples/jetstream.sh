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
JS_PORT="${JS_PORT:-15889}"
JS_URL="${JS_URL:-nats://127.0.0.1:$JS_PORT}"
export JS_URL
COUNT="${COUNT:-1000}"

nats-server -js -sd "$TMP/js-store" -p "$JS_PORT" >"$TMP/js-server.log" 2>&1 &
JS_PID=$!
EXTRA_PIDS+=("$JS_PID")

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

JS_URL="$JS_URL" COUNT="$COUNT" "$ROOT/examples/jetstream-populate.sh" >"$TMP/populate.log"

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
    "Populated stream SENSORS with $COUNT historical js.sensors.temp events before monoblok started." \
    "monoblok replayed the stream before opening its listener, so late LVC subscribers see warm replay-derived state." \
    "A final live event was published after startup and appears on js.live.temp." \
    "The raw js.sensors.temp source remains private patchbay ingress."

show "populate"                       populate.log
show "monoblok JetStream startup"      daemon.log 20
show "late LVC view of replay outputs" lvc.txt
show "live output after catch-up"       live.txt
show "local raw source subscription"   raw.txt
