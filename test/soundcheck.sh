#!/bin/sh
set -eu

bin="${1:?usage: soundcheck.sh /path/to/monoblok}"
tmp="${TMPDIR:-/tmp}/monoblok-soundcheck-$$"
mkdir -p "$tmp"
cleanup() {
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

out="$(printf 'sensors.temp|31\n' | "$bin" --soundcheck --soundcheck-label examples/strict-vectors.edn)"

printf '%s\n' "$out" | grep '^in|sensors.temp|31$' >/dev/null
printf '%s\n' "$out" | grep '^out|sensors.temp.seen|31$' >/dev/null

json_out="$(printf 'sensors.temp|{"temp":31,"status":"warm"}\n' | "$bin" --soundcheck --soundcheck-label examples/strict-vectors.json)"
printf '%s\n' "$json_out" | grep '^in|sensors.temp|{"temp":31,"status":"warm"}$' >/dev/null
printf '%s\n' "$json_out" | grep '^out|sensors.temp.seen|{"temp":31,"status":"warm"}$' >/dev/null
printf '%s\n' "$json_out" | grep '^out|sensors.temp.temp|31$' >/dev/null
printf '%s\n' "$json_out" | grep '^out|sensors.temp.state|warm$' >/dev/null

linger_patchbay="$tmp/linger.edn"
cat > "$linger_patchbay" <<'EOF'
(on "sensors.*"
  (debounce! :ms 25
    (subject-append "settled")
    payload))
EOF

linger_out="$(printf 'sensors.temp|31\n' | "$bin" --soundcheck --soundcheck-label --soundcheck-linger-ms 250 "$linger_patchbay")"
printf '%s\n' "$linger_out" | grep '^in|sensors.temp|31$' >/dev/null
printf '%s\n' "$linger_out" | grep '^out|sensors.temp.settled|31$' >/dev/null

no_linger_out="$(printf 'sensors.temp|31\n' | "$bin" --soundcheck --soundcheck-label --soundcheck-linger-ms 0 "$linger_patchbay")"
printf '%s\n' "$no_linger_out" | grep '^in|sensors.temp|31$' >/dev/null
if printf '%s\n' "$no_linger_out" | grep '^out|sensors.temp.settled|31$' >/dev/null; then
    echo "soundcheck linger disabled but timer output was emitted" >&2
    exit 1
fi

printf 'soundcheck passed\n'
