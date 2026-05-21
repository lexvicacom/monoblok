#!/usr/bin/env bash
set -euo pipefail

JS_PORT="${JS_PORT:-15889}"
JS_URL="${JS_URL:-nats://127.0.0.1:$JS_PORT}"
STREAM="${STREAM:-SENSORS}"
STREAM_SUBJECTS="${STREAM_SUBJECTS:-js.sensors.>}"
SUBJECT="${SUBJECT:-js.sensors.temp}"
COUNT="${COUNT:-1000}"

if ! command -v nats >/dev/null; then
    echo "the nats CLI is required (https://github.com/nats-io/natscli)" >&2
    exit 1
fi

case "$COUNT" in
    ''|*[!0-9]*)
        echo "COUNT must be a non-negative integer" >&2
        exit 1
        ;;
esac

nats --no-context -s "$JS_URL" stream rm "$STREAM" --force >/dev/null 2>&1 || true
nats --no-context -s "$JS_URL" stream add "$STREAM" \
    --subjects "$STREAM_SUBJECTS" \
    --storage file \
    --defaults >/dev/null

python3 - "$JS_URL" "$SUBJECT" "$COUNT" <<'PY'
import math
import socket
import sys
from urllib.parse import urlparse

url = sys.argv[1]
subject = sys.argv[2].encode("ascii")
count = int(sys.argv[3])

parsed = urlparse(url if "://" in url else f"nats://{url}")
host = parsed.hostname or "127.0.0.1"
port = parsed.port or 4222

sock = socket.create_connection((host, port), timeout=5)
sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
sock.sendall(b"CONNECT {}\r\n")

for i in range(1, count + 1):
    # Slow drift plus a small wave gives replay something more realistic than
    # a constant payload while keeping the stream deterministic.
    value = 20.0 + (i / max(count, 1)) * 5.0 + math.sin(i / 17.0)
    payload = f"{value:.3f}".encode("ascii")
    frame = b"PUB " + subject + b" " + str(len(payload)).encode("ascii") + b"\r\n" + payload + b"\r\n"
    sock.sendall(frame)

sock.sendall(b"PING\r\n")
buf = b""
sock.settimeout(5)
while b"PONG\r\n" not in buf:
    chunk = sock.recv(4096)
    if not chunk:
        raise RuntimeError("server closed before PONG")
    buf += chunk
sock.close()
PY

echo "populated stream=$STREAM subject=$SUBJECT count=$COUNT url=$JS_URL"
