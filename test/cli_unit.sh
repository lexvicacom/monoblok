#!/bin/sh
set -eu

bin="${1:?usage: cli_unit.sh /path/to/monoblok}"
tmp="${TMPDIR:-/tmp}/monoblok-cli-$$"
mkdir -p "$tmp"

cleanup() {
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

valid="$tmp/valid.edn"
valid_config="$tmp/valid-config.edn"
valid_nested_import="$tmp/valid-nested-import.edn"
bad_lvc="$tmp/bad-lvc.edn"
bad_export="$tmp/bad-export.edn"
bad_import="$tmp/bad-import.edn"
bad_on="$tmp/bad-on.edn"
bad_eval="$tmp/bad-eval.edn"
bad_parse="$tmp/bad-parse.edn"

cat > "$valid" <<'EOF'
(on "sensors.*" (publish! (subject-append "seen") payload))
(on "skip.*" (publish! "skip.out" payload))
EOF

cat > "$valid_config" <<'EOF'
(lvc [">" "devices.*"])
(export :servers ["nats://127.0.0.1:4222"]
        :export "telemetry.>"
        :tls true
        :tls-skip-verify false
        :origin-header true
        :connect-timeout-ms 100
        :ping-interval-ms 1000
        :reconnect-wait-ms 250
        :max-reconnect 2
        :name "monoblok-test"
        :creds "creds.creds"
        :user "u"
        :password "p"
        :token "t"
        :tls-ca "ca.pem"
        :tls-cert "cert.pem"
        :tls-key "key.pem")
(import :servers ["nats://127.0.0.1:4223"]
        :subject ["raw.>"]
        :max-pending 64
        :name "monoblok-import-test"
        :origin-header true)
(on "sensors.*" :reentrant true (publish! (subject-append "seen") payload))
EOF

cat > "$valid_nested_import" <<'EOF'
(import
  :core [[ :servers ["nats://127.0.0.1:4223"]
           :subject ["raw.>"] ]]
  :streams [[ :servers ["nats://127.0.0.1:4222"]
              :subject "sensors.temp"
              :stream "SENSORS"
              :consumer "monoblok-test"
              :catch-up true ]])
(on "sensors.temp" (if replaying? (publish! "replay.temp" payload) (publish! "live.temp" payload)))
EOF

cat > "$bad_lvc" <<'EOF'
(lvc [])
EOF

cat > "$bad_export" <<'EOF'
(export :export ["telemetry.>"])
EOF

cat > "$bad_import" <<'EOF'
(import :servers ["nats://127.0.0.1:4222"])
EOF

cat > "$bad_on" <<'EOF'
(on "sensors.*" :unknown true (publish! "out" payload))
EOF

cat > "$bad_eval" <<'EOF'
(on "sensors.*" (round payload))
EOF

cat > "$bad_parse" <<'EOF'
(on "sensors.*" (publish! "out" payload)
EOF

"$bin" --help > "$tmp/help.out"
grep '^Usage:' "$tmp/help.out" >/dev/null
"$bin" --version > "$tmp/version.out"
grep '^monoblok ' "$tmp/version.out" >/dev/null

if "$bin" --validate > "$tmp/missing-validate.out" 2>&1; then
    echo "validate without patchbay unexpectedly succeeded" >&2
    exit 1
fi
grep '^Usage:' "$tmp/missing-validate.out" >/dev/null

"$bin" --validate "$valid" > "$tmp/validate.out" 2> "$tmp/validate.err"
grep ': ok (2 rules)' "$tmp/validate.out" >/dev/null
"$bin" --validate "$valid_config" > "$tmp/validate-config.out" 2> "$tmp/validate-config.err"
grep ': ok (1 rule)' "$tmp/validate-config.out" >/dev/null
"$bin" --validate "$valid_nested_import" > "$tmp/validate-nested-import.out" 2> "$tmp/validate-nested-import.err"
grep ': ok (1 rule)' "$tmp/validate-nested-import.out" >/dev/null

if "$bin" --validate "$bad_lvc" > "$tmp/bad-lvc.out" 2> "$tmp/bad-lvc.err"; then
    echo "invalid lvc unexpectedly validated" >&2
    exit 1
fi
grep 'lvc vector must not be empty' "$tmp/bad-lvc.err" >/dev/null

if "$bin" --validate "$bad_export" > "$tmp/bad-export.out" 2> "$tmp/bad-export.err"; then
    echo "invalid export unexpectedly validated" >&2
    exit 1
fi
grep 'export requires :servers' "$tmp/bad-export.err" >/dev/null

if "$bin" --validate "$bad_import" > "$tmp/bad-import.out" 2> "$tmp/bad-import.err"; then
    echo "invalid import unexpectedly validated" >&2
    exit 1
fi
grep 'import requires :subject' "$tmp/bad-import.err" >/dev/null

if "$bin" --validate "$bad_on" > "$tmp/bad-on.out" 2> "$tmp/bad-on.err"; then
    echo "invalid on options unexpectedly validated" >&2
    exit 1
fi
grep 'unknown on option' "$tmp/bad-on.err" >/dev/null

printf 'sensors.temp|31\nother.temp|40\n\n' | "$bin" --soundcheck "$valid" > "$tmp/soundcheck.out" 2> "$tmp/soundcheck.err"
grep '^sensors.temp|31$' "$tmp/soundcheck.out" >/dev/null
grep '^sensors.temp.seen|31$' "$tmp/soundcheck.out" >/dev/null
grep '^other.temp|40$' "$tmp/soundcheck.out" >/dev/null

printf 'sensors.temp|31\n' | "$bin" --soundcheck --soundcheck-label "$valid" > "$tmp/soundcheck-label.out" 2> "$tmp/soundcheck-label.err"
grep '^in|sensors.temp|31$' "$tmp/soundcheck-label.out" >/dev/null
grep '^out|sensors.temp.seen|31$' "$tmp/soundcheck-label.out" >/dev/null

if printf 'badrow\n' | "$bin" --soundcheck "$valid" > "$tmp/soundcheck-bad-row.out" 2> "$tmp/soundcheck-bad-row.err"; then
    echo "bad soundcheck row unexpectedly succeeded" >&2
    exit 1
fi
grep 'expected SUBJECT|payload' "$tmp/soundcheck-bad-row.err" >/dev/null

if printf 'sensors.temp|bad\n' | "$bin" --soundcheck "$bad_eval" > "$tmp/soundcheck-bad-eval.out" 2> "$tmp/soundcheck-bad-eval.err"; then
    echo "bad soundcheck eval unexpectedly succeeded" >&2
    exit 1
fi
grep 'soundcheck: eval error' "$tmp/soundcheck-bad-eval.err" >/dev/null

if printf 'sensors.temp|31\n' | "$bin" --soundcheck "$bad_parse" > "$tmp/soundcheck-bad-parse.out" 2> "$tmp/soundcheck-bad-parse.err"; then
    echo "bad soundcheck parse unexpectedly succeeded" >&2
    exit 1
fi
grep 'soundcheck: parse error' "$tmp/soundcheck-bad-parse.err" >/dev/null

printf 'cli tests passed\n'
