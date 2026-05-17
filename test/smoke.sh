#!/bin/sh
set -eu

bin="${1:?usage: smoke.sh /path/to/monoblok}"
port="${MONOBLOK_PORT:-42424}"
tmp="${TMPDIR:-/tmp}/monoblok-smoke-$$"
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sub_in="$tmp/sub.in"
sub_out="$tmp/sub.out"
pub_in="$tmp/pub.in"
pub_out="$tmp/pub.out"
srv_out="$tmp/server.out"
patchbay="$tmp/patchbay.edn"

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
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

"$bin" "$patchbay" --host 127.0.0.1 --port "$port" --stats-tick-ms 50 >"$srv_out" 2>&1 &
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
printf 'smoke passed\n'
