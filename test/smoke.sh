#!/bin/sh
set -eu

bin="${1:?usage: smoke.sh /path/to/monoblok}"
port="${MONOBLOK_PORT:-42424}"
tls_port="${MONOBLOK_TLS_PORT:-42425}"
auth_port="${MONOBLOK_AUTH_PORT:-42426}"
auth_user_port="${MONOBLOK_AUTH_USER_PORT:-42427}"
http_port="${MONOBLOK_HTTP_PORT:-42428}"
auth_http_port="${MONOBLOK_AUTH_HTTP_PORT:-42429}"
tmp="${TMPDIR:-/tmp}/monoblok-smoke-$$"
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sub_in="$tmp/sub.in"
sub_out="$tmp/sub.out"
pub_in="$tmp/pub.in"
pub_out="$tmp/pub.out"
srv_out="$tmp/server.out"
tls_srv_out="$tmp/tls-server.out"
auth_srv_out="$tmp/auth-server.out"
patchbay="$tmp/patchbay.edn"
tls_cert="$tmp/cert.pem"
tls_key="$tmp/key.pem"

mkdir -p "$tmp"
mkfifo "$sub_in" "$pub_in"
cat > "$patchbay" <<'EOF'
(lvc [">"])

(on "sensors.*"
  (when (contains? ["temp" "hum" "batt"] (subject-token 1))
    (publish! (subject-append "seen") payload)))
EOF

cleanup() {
    set +e
    [ "${sub_pid:-}" ] && kill "$sub_pid" 2>/dev/null
    [ "${pub_pid:-}" ] && kill "$pub_pid" 2>/dev/null
    [ "${srv_pid:-}" ] && kill "$srv_pid" 2>/dev/null
    [ "${tls_srv_pid:-}" ] && kill "$tls_srv_pid" 2>/dev/null
    [ "${auth_pid:-}" ] && kill "$auth_pid" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

"$bin" "$patchbay" --host 127.0.0.1 --port "$port" --http-port "$http_port" --stats-tick-ms 50 >"$srv_out" 2>&1 &
srv_pid=$!
sleep 0.2

nc 127.0.0.1 "$port" <"$sub_in" >"$sub_out" &
sub_pid=$!
exec 3>"$sub_in"
printf 'SUB foo 1\r\n' >&3
printf 'SUB sensors.temp.seen 2\r\n' >&3
printf 'SUB $STATS.> 3\r\n' >&3
sleep 0.1

nc 127.0.0.1 "$port" <"$pub_in" >"$pub_out" &
pub_pid=$!
exec 4>"$pub_in"
printf 'PING\r\nPUB foo _INBOX.7 2\r\nhi\r\nPUB sensors.temp 2\r\n31\r\nPUB $STATS.bad 1\r\nx\r\n' >&4
sleep 0.3

grep 'MSG foo 1 _INBOX.7 2' "$sub_out" >/dev/null
grep 'hi' "$sub_out" >/dev/null
grep 'MSG sensors.temp.seen 2 2' "$sub_out" >/dev/null
grep '31' "$sub_out" >/dev/null
grep 'MSG $STATS.global.pubs 3 1' "$sub_out" >/dev/null
grep 'MSG $STATS.rules.0.emitted 3 1' "$sub_out" >/dev/null
grep 'MSG $STATS.rules.0.suppressed 3 1' "$sub_out" >/dev/null
grep '\$STATS is read-only' "$pub_out" >/dev/null
grep 'info: loaded 1 patchbay form(s)' "$srv_out" >/dev/null

python3 - "$http_port" <<'PY'
import http.client
import socket
import sys

port = int(sys.argv[1])

def recv_until(sock, needle):
    data = b""
    while needle not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError(f"eof before {needle!r}: {data!r}")
        data += chunk
    return data

def latest_request(path):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request("GET", path)
    res = conn.getresponse()
    out = {
        "status": res.status,
        "content_type": res.getheader("Content-Type"),
        "body": res.read(),
    }
    conn.close()
    return out

sse = socket.create_connection(("127.0.0.1", port), timeout=5)
sse.sendall(b"GET /sub/sensors/temp/seen HTTP/1.1\r\nHost: localhost\r\n\r\n")
headers = recv_until(sse, b"\r\n\r\n")
if b"200 OK" not in headers or b"text/event-stream" not in headers or b"Server: monoblok" not in headers:
    raise RuntimeError(f"bad SSE headers: {headers!r}")

pub = socket.create_connection(("127.0.0.1", port), timeout=5)
pub.sendall(
    b"POST /pub/sensors/temp HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: 2\r\n"
    b"\r\n"
    b"44"
)
resp = recv_until(pub, b"\r\n\r\n")
if b"202 Accepted" not in resp or b"Server: monoblok" not in resp:
    raise RuntimeError(f"bad POST response: {resp!r}")
pub.close()

event = recv_until(sse, b"\n\n")
if b'event: msg\n' not in event or b'"subject":"sensors.temp.seen"' not in event or b'"payload":"44"' not in event:
    raise RuntimeError(f"bad SSE event: {event!r}")
sse.close()

latest = latest_request("/latest/sensors/temp")
if latest["status"] != 200 or latest["content_type"] != "application/octet-stream" or latest["body"] != b"44":
    raise RuntimeError(f"bad latest response: {latest!r}")

missing = latest_request("/latest/missing/value")
if missing["status"] != 404 or missing["body"] != b"not cached\n":
    raise RuntimeError(f"bad missing latest response: {missing!r}")

bad = socket.create_connection(("127.0.0.1", port), timeout=5)
bad.sendall(
    b"POST /pub/sensors/temp HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Content-Type: application/octet-stream\r\n"
    b"Content-Length: 1\r\n"
    b"\r\n"
    b"x"
)
resp = recv_until(bad, b"\r\n\r\n")
if b"415 Unsupported Media Type" not in resp:
    raise RuntimeError(f"bad binary content-type response: {resp!r}")
bad.close()
PY

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tls_key" -out "$tls_cert" -subj /CN=localhost -days 1 >/dev/null 2>&1
"$bin" --host 127.0.0.1 --port "$tls_port" --tls-cert "$tls_cert" --tls-key "$tls_key" --stats-tick-ms 100000 >"$tls_srv_out" 2>&1 &
tls_srv_pid=$!
sleep 0.2
python3 - "$tls_port" <<'PY'
import socket
import ssl
import sys

port = int(sys.argv[1])
sock = socket.create_connection(("127.0.0.1", port), timeout=5)
info = b""
while not info.endswith(b"\n"):
    chunk = sock.recv(1)
    if not chunk:
        raise RuntimeError("eof before INFO")
    info += chunk
if b'"tls_required":true' not in info:
    raise RuntimeError(f"INFO did not advertise TLS: {info!r}")

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
tls = ctx.wrap_socket(sock, server_hostname="localhost", do_handshake_on_connect=True)
tls.sendall(b"CONNECT {}\r\nPING\r\n")
data = b""
while b"PONG\r\n" not in data:
    chunk = tls.recv(4096)
    if not chunk:
        raise RuntimeError("eof before PONG")
    data += chunk
tls.close()
PY
kill "$tls_srv_pid" 2>/dev/null || true
wait "$tls_srv_pid" 2>/dev/null || true
unset tls_srv_pid

MB_SMOKE_AUTH_TOKEN='sekret' "$bin" --host 127.0.0.1 --port "$auth_port" --http-port "$auth_http_port" --auth-token-env MB_SMOKE_AUTH_TOKEN --stats-tick-ms 100000 >"$auth_srv_out" 2>&1 &
auth_pid=$!
sleep 0.2
python3 - "$auth_port" "$auth_http_port" <<'PY'
import socket
import sys

port = int(sys.argv[1])
http_port = int(sys.argv[2])

def read_line(sock):
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            raise RuntimeError("eof before line")
        data += chunk
    return data

def connect():
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    info = read_line(sock)
    if b'"auth_required":true' not in info:
        raise RuntimeError(f"INFO did not advertise auth: {info!r}")
    return sock

sock = connect()
sock.sendall(b"PING\r\n")
data = sock.recv(4096)
if b"PONG\r\n" in data:
    raise RuntimeError("unauthenticated PING got PONG")
sock.close()

sock = connect()
sock.sendall(b'CONNECT {"auth_token":"wrong"}\r\nPING\r\n')
data = sock.recv(4096)
if b"PONG\r\n" in data:
    raise RuntimeError("bad token got PONG")
sock.close()

sock = connect()
sock.sendall(b'CONNECT {"auth_token":"sekret"}\r\nPING\r\n')
data = b""
while b"PONG\r\n" not in data:
    chunk = sock.recv(4096)
    if not chunk:
        raise RuntimeError("eof before authenticated PONG")
    data += chunk
sock.close()

http = socket.create_connection(("127.0.0.1", http_port), timeout=5)
http.sendall(
    b"POST /pub/http/auth HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: 2\r\n"
    b"\r\n"
    b"ok"
)
data = read_line(http)
if b"401 Unauthorized" not in data:
    raise RuntimeError(f"unauthenticated HTTP POST did not get 401: {data!r}")
http.close()

http = socket.create_connection(("127.0.0.1", http_port), timeout=5)
http.sendall(
    b"POST /pub/http/auth HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Authorization: Bearer sekret\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: 2\r\n"
    b"\r\n"
    b"ok"
)
data = read_line(http)
if b"202 Accepted" not in data:
    raise RuntimeError(f"authenticated HTTP POST did not get 202: {data!r}")
http.close()
PY
kill "$auth_pid" 2>/dev/null || true
wait "$auth_pid" 2>/dev/null || true
unset auth_pid

MB_SMOKE_AUTH_USER='alice' MB_SMOKE_AUTH_PASS='wonder' "$bin" --host 127.0.0.1 --port "$auth_user_port" --http-port "$auth_http_port" --auth-user-env MB_SMOKE_AUTH_USER --auth-pass-env MB_SMOKE_AUTH_PASS --stats-tick-ms 100000 >"$auth_srv_out" 2>&1 &
auth_pid=$!
sleep 0.2
python3 - "$auth_user_port" "$auth_http_port" <<'PY'
import socket
import sys

port = int(sys.argv[1])
http_port = int(sys.argv[2])
sock = socket.create_connection(("127.0.0.1", port), timeout=5)
info = b""
while not info.endswith(b"\n"):
    chunk = sock.recv(1)
    if not chunk:
        raise RuntimeError("eof before INFO")
    info += chunk
if b'"auth_required":true' not in info:
    raise RuntimeError(f"INFO did not advertise auth: {info!r}")
sock.sendall(b'CONNECT {"user":"alice","pass":"wonder"}\r\nPING\r\n')
data = b""
while b"PONG\r\n" not in data:
    chunk = sock.recv(4096)
    if not chunk:
        raise RuntimeError("eof before authenticated PONG")
    data += chunk
sock.close()

http = socket.create_connection(("127.0.0.1", http_port), timeout=5)
http.sendall(
    b"POST /pub/http/basic HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Authorization: Basic YWxpY2U6d29uZGVy\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: 2\r\n"
    b"\r\n"
    b"ok"
)
status = b""
while not status.endswith(b"\n"):
    chunk = http.recv(1)
    if not chunk:
        raise RuntimeError("eof before HTTP basic response")
    status += chunk
if b"202 Accepted" not in status:
    raise RuntimeError(f"authenticated HTTP basic POST did not get 202: {status!r}")
http.close()
PY
kill "$auth_pid" 2>/dev/null || true
wait "$auth_pid" 2>/dev/null || true
unset auth_pid

printf 'smoke passed\n'
