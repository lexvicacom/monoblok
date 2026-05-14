#!/bin/sh
set -eu

bin="${1:?usage: soundcheck.sh /path/to/monoblok-c}"

out="$(printf 'sensors.temp|31\n' | "$bin" --soundcheck --soundcheck-label c-spike/examples/strict-vectors.edn)"

printf '%s\n' "$out" | grep '^in|sensors.temp|31$' >/dev/null
printf '%s\n' "$out" | grep '^out|sensors.temp.seen|31$' >/dev/null

json_out="$(printf 'sensors.temp|{"temp":31,"status":"warm"}\n' | "$bin" --soundcheck --soundcheck-label c-spike/examples/strict-vectors.json)"
printf '%s\n' "$json_out" | grep '^in|sensors.temp|{"temp":31,"status":"warm"}$' >/dev/null
printf '%s\n' "$json_out" | grep '^out|sensors.temp.seen|{"temp":31,"status":"warm"}$' >/dev/null
printf '%s\n' "$json_out" | grep '^out|sensors.temp.temp|31$' >/dev/null
printf '%s\n' "$json_out" | grep '^out|sensors.temp.state|warm$' >/dev/null

printf 'soundcheck passed\n'
