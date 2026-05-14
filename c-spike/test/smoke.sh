#!/bin/sh
set -eu

bin="${1:?usage: smoke.sh /path/to/monoblok-c}"
port="${MONOBLOK_C_PORT:-42424}"
tmp="${TMPDIR:-/tmp}/monoblok-c-smoke-$$"
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sub_in="$tmp/sub.in"
sub_out="$tmp/sub.out"
pub_in="$tmp/pub.in"
pub_out="$tmp/pub.out"
srv_out="$tmp/server.out"

mkdir -p "$tmp"
mkfifo "$sub_in" "$pub_in"

cleanup() {
    set +e
    [ "${sub_pid:-}" ] && kill "$sub_pid" 2>/dev/null
    [ "${pub_pid:-}" ] && kill "$pub_pid" 2>/dev/null
    [ "${srv_pid:-}" ] && kill "$srv_pid" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

"$bin" "$root/examples/strict-vectors.edn" --host 127.0.0.1 --port "$port" >"$srv_out" 2>&1 &
srv_pid=$!
sleep 0.2

nc 127.0.0.1 "$port" <"$sub_in" >"$sub_out" &
sub_pid=$!
exec 3>"$sub_in"
printf 'SUB foo 1\r\n' >&3
printf 'SUB sensors.temp.seen 2\r\n' >&3
sleep 0.1

nc 127.0.0.1 "$port" <"$pub_in" >"$pub_out" &
pub_pid=$!
exec 4>"$pub_in"
printf 'PING\r\nPUB foo 2\r\nhi\r\nPUB sensors.temp 2\r\n31\r\n' >&4
sleep 0.3

grep 'MSG foo 1 2' "$sub_out" >/dev/null
grep 'hi' "$sub_out" >/dev/null
grep 'MSG sensors.temp.seen 2 2' "$sub_out" >/dev/null
grep '31' "$sub_out" >/dev/null
grep 'info: loaded 1 patchbay form(s)' "$srv_out" >/dev/null
printf 'smoke passed\n'
