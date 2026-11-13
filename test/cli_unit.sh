#!/bin/sh
set -eu

bin="${1:?usage: cli_unit.sh /path/to/monoblok}"
tmp="${TMPDIR:-/tmp}/monoblok-cli-$$"
root="$(cd "$(dirname "$0")/.." && pwd)"
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
        :replay-header true
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

# Config checks go through the same loader as startup: every case must be rejected by both.
check_bad_config() {
    name="$1"; want="$2"; body="$3"
    printf '%s\n' "$body" > "$tmp/cfg-$name.edn"
    if "$bin" --validate "$tmp/cfg-$name.edn" > "$tmp/cfg-$name.out" 2> "$tmp/cfg-$name.err"; then
        echo "bad config $name unexpectedly validated" >&2
        exit 1
    fi
    grep "$want" "$tmp/cfg-$name.err" >/dev/null
}
check_bad_config unknown-export-opt 'unknown export option: bogus' '(export :servers ["nats://x:4222"] :bogus 1)'
check_bad_config export-bool 'export :tls expects boolean' '(export :servers ["nats://x:4222"] :tls 1)'
check_bad_config export-number 'export :max-reconnect expects number' '(export :servers ["nats://x:4222"] :max-reconnect "a")'
check_bad_config dup-export 'duplicate export/bridge form' '(export :servers ["nats://x:4222"])
(bridge :servers ["nats://y:4222"])'
check_bad_config env-empty 'import :name must not be empty' '(import :servers ["nats://x:4222"] :subject "a.>" :name (env "MONOBLOK_CLI_UNSET_ENV"))'
check_bad_config max-pending 'max-pending must be a positive integer' '(import :servers ["nats://x:4222"] :subject "a.>" :max-pending 0)'
check_bad_config stream-no-consumer 'requires :consumer' '(import :streams [[:servers ["nats://x:4222"] :subject "a.>" :stream "S"]])'
check_bad_config catch-up-on-core 'unknown import option: catch-up' '(import :servers ["nats://x:4222"] :subject "a.>" :catch-up true)'

# Regex forms: the pattern is compiled at load, so --validate catches bad ones.
good_regex="$tmp/good-regex.edn"
cat > "$good_regex" <<'EOF'
(on "in.>" (publish! "out.t" (re-capture "t=(\\d+)" payload)))
EOF
"$bin" --validate "$good_regex" >/dev/null
printf 'in.a|id=1 t=42\n' | "$bin" --soundcheck "$good_regex" | grep '^out.t|42$' >/dev/null

# The #"..." literal and single-quoted YAML scalars carry the pattern raw.
literal_regex="$tmp/literal-regex.edn"
cat > "$literal_regex" <<'EOF'
(on "in.>" (publish! "out.t" (re-capture #"t=(\d+)" payload)))
EOF
"$bin" --validate "$literal_regex" >/dev/null
printf 'in.a|id=1 t=42\n' | "$bin" --soundcheck "$literal_regex" | grep '^out.t|42$' >/dev/null
yaml_regex="$tmp/regex-single.yml"
cat > "$yaml_regex" <<'EOF'
on:
  - sub: "in.>"
    do: [publish!, "out.t", [re-capture, 't=(\d+)', $payload]]
EOF
"$bin" --validate "$yaml_regex" >/dev/null
printf 'in.a|id=1 t=42\n' | "$bin" --soundcheck "$yaml_regex" | grep '^out.t|42$' >/dev/null

# A pattern too big for the fast table still validates, with a note.
slow_regex="$tmp/slow-regex.edn"
printf '%s\n' '(on "a" (publish! "b" (matches? "(?:a|b)*a(?:a|b){100}c" payload)))' > "$slow_regex"
"$bin" --validate "$slow_regex" 2> "$tmp/slow-regex.err" >/dev/null
grep '^note: rule 0: matches?: pattern runs without the fast table' "$tmp/slow-regex.err" >/dev/null
if "$bin" --validate "$good_regex" 2>&1 >/dev/null | grep '^note:' >/dev/null; then
    echo "fast-table pattern should not produce a note" >&2
    exit 1
fi

check_bad_regex() {
    name="$1"; want="$2"; body="$3"
    printf '%s\n' "$body" > "$tmp/regex-$name.edn"
    if "$bin" --validate "$tmp/regex-$name.edn" > "$tmp/regex-$name.out" 2> "$tmp/regex-$name.err"; then
        echo "bad regex $name unexpectedly validated" >&2
        exit 1
    fi
    grep "$want" "$tmp/regex-$name.err" >/dev/null
}
check_bad_regex bad-pattern 'matches?: bad pattern: .* at byte 2 in "ab(cd"' '(on "a" (publish! "b" (matches? "ab(cd" payload)))'
check_bad_regex non-literal 'matches?: pattern must be a string literal' '(on "a" (publish! "b" (matches? payload payload)))'
check_bad_regex bad-brace 'bad pattern: .*write .{ for a literal brace' '(on "a" (publish! "b" (matches? "{x" payload)))'
check_bad_regex computed 're-replace: pattern must be a string literal' '(on "a" (publish! "b" (re-replace (str-concat "a" "b") "x" payload)))'

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

# Failed startup must drain initialized handles without writing a snapshot.
rc=0
"$bin" --host invalid --port 0 --patchbay "$valid" --snapshot "$tmp/startup.snapshot" \
    > "$tmp/startup.out" 2> "$tmp/startup.err" || rc=$?
test "$rc" -eq 1
grep 'invalid address' "$tmp/startup.err" >/dev/null
test ! -e "$tmp/startup.snapshot"
if grep -E 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:' "$tmp/startup.err" >/dev/null; then
    cat "$tmp/startup.err" >&2
    exit 1
fi

# --validate checks every branch of a rule body for unknown forms.
bogus_branch="$tmp/bogus-branch.edn"
cat > "$bogus_branch" <<'EOF'
(on "t.>" (when (> payload-float 5) (bogus 1 2)))
EOF
if "$bin" --validate "$bogus_branch" > "$tmp/bogus.out" 2> "$tmp/bogus.err"; then
    echo "validate accepted an unknown form in an untaken branch" >&2
    exit 1
fi
grep 'unknown form: bogus' "$tmp/bogus.err" >/dev/null

# hysteresis, ewma, and last validate; bad shapes do not.
good_forms="$tmp/good-forms.edn"
cat > "$good_forms" <<'EOF'
(lvc ["temp.*"])
(on "temp.*" (publish! "temp.avg" (ewma 0.5 (moving-avg 10 payload-float))))
(on "temp.*" (when (hysteresis 20 25 payload-float) (publish! "alarm" (last "temp.outside"))))
EOF
"$bin" --validate "$good_forms" > "$tmp/good-forms.out" 2> "$tmp/good-forms.err"
grep ': ok (2 rules)' "$tmp/good-forms.out" >/dev/null

for bad in '(hysteresis 25 20 payload-float)' '(ewma 0 payload-float)' '(last "temp.*")'; do
    printf '(on "t.>" %s)\n' "$bad" > "$tmp/bad-form.edn"
    if "$bin" --validate "$tmp/bad-form.edn" > "$tmp/bad-form.out" 2> "$tmp/bad-form.err"; then
        echo "validate accepted bad form: $bad" >&2
        exit 1
    fi
done

# keyed was removed and is now an unknown form.
printf '(on "t.>" (keyed "k" 1))\n' > "$tmp/keyed.edn"
if "$bin" --validate "$tmp/keyed.edn" > "$tmp/keyed.out" 2> "$tmp/keyed.err"; then
    echo "validate accepted removed form keyed" >&2
    exit 1
fi
grep 'unknown form: keyed' "$tmp/keyed.err" >/dev/null

# YAML: bare subject is the :subject option; [publish!, subject, ...] gets a $subject hint.
cat > "$tmp/bare-subject.yml" <<'EOF'
on:
  - sub: "t.>"
    form: [publish!, subject, $payload]
EOF
if "$bin" --validate "$tmp/bare-subject.yml" > "$tmp/bare-subject.out" 2> "$tmp/bare-subject.err"; then
    echo "validate accepted bare subject after publish!" >&2
    exit 1
fi
grep 'use \$subject' "$tmp/bare-subject.err" >/dev/null
cat > "$tmp/count-subject.yml" <<'EOF'
on:
  - sub: "t.>"
    form: [count!, subject, "x.count"]
EOF
"$bin" --validate "$tmp/count-subject.yml" > /dev/null 2>&1

# every, reply!, and reply-to validate in EDN, JSON, and YAML; bad every shapes do not.
every_edn="$tmp/every.edn"
cat > "$every_edn" <<'EOF'
(lvc "temp.*")
(every :ms 1000 :reentrant false (publish! "tick" (last "temp.a")))
(on "q" (reply! (if reply-to (last "temp.a") nil)))
(on "q.dp" (-> payload-float (round 1) (reply! :dp 1)))
EOF
"$bin" --validate "$every_edn" > "$tmp/every.out" 2> "$tmp/every.err"
grep ': ok (3 rules)' "$tmp/every.out" >/dev/null

every_json="$tmp/every.json"
cat > "$every_json" <<'EOF'
[
  ["lvc", "temp.*"],
  ["every", {"ms": 1000}, ["publish!", "tick", ["last", "temp.a"]]],
  ["on", "q", ["reply!", ["if", "$reply-to", ["last", "temp.a"], null]]],
  ["on", "temp.*", ["publish!", "sm", ["ewma", 0.5, "$payload-float"]]],
  ["on", "temp.*", ["publish!", "alarm", ["hysteresis", 20, 25, "$payload-float"]]]
]
EOF
"$bin" --validate "$every_json" > "$tmp/every-json.out" 2> "$tmp/every-json.err"
grep ': ok (4 rules)' "$tmp/every-json.out" >/dev/null

every_yaml="$tmp/every.yml"
cat > "$every_yaml" <<'EOF'
lvc: "temp.*"
every:
  - ms: 1000
    form: [publish!, "tick", [last, "temp.a"]]
on:
  - sub: "q"
    form: [reply!, [if, $reply-to, [last, "temp.a"], null]]
  - sub: "temp.*"
    form: [publish!, "sm", [ewma, 0.5, $payload-float]]
  - sub: "temp.*"
    form: [publish!, "alarm", [hysteresis, 20, 25, $payload-float]]
EOF
"$bin" --validate "$every_yaml" > "$tmp/every-yaml.out" 2> "$tmp/every-yaml.err"
grep ': ok (4 rules)' "$tmp/every-yaml.out" >/dev/null

# every rules that do not touch the payload validate with the empty sample payload.
every_yaml_when="$tmp/every-when.yml"
cat > "$every_yaml_when" <<'EOF'
every:
  - ms: 500
    when: [not, $replaying]
    form: [publish!, "tick", ["contains?", [vector, 1, 2, 3], 2]]
EOF
"$bin" --validate "$every_yaml_when" > "$tmp/every-when.out" 2> "$tmp/every-when.err"
grep ': ok (1 rule)' "$tmp/every-when.out" >/dev/null

# YAML/JSON load errors: each removed or unsafe spelling is rejected with a message naming the fix.
expect_load_error() {
    name="$1"; needle="$2"; body="$3"
    printf '%s\n' "$body" > "$tmp/$name"
    if "$bin" --validate "$tmp/$name" > "$tmp/load-err.out" 2> "$tmp/load-err.err"; then
        echo "validate accepted $name" >&2
        exit 1
    fi
    if ! grep -F -- "$needle" "$tmp/load-err.err" >/dev/null; then
        echo "validate error for $name missing '$needle':" >&2
        cat "$tmp/load-err.err" >&2
        exit 1
    fi
}
expect_load_error bad-dq-escape.yml 'use single quotes for raw text' 'on:
  - sub: "in.>"
    do: [publish!, "out.t", [re-capture, "t=(\d+)", $payload]]'
expect_load_error bad-bare.yml 'use $payload' 'on:
  - sub: "a"
    form: [publish!, "x", payload]'
expect_load_error bad-typo.yml 'unquoted word "payload-flaot"' 'on:
  - sub: "a"
    form: [publish!, "x", payload-flaot]'
expect_load_error bad-binding.yml 'unknown binding $payload-flaot' 'on:
  - sub: "a"
    form: [publish!, "x", $payload-flaot]'
expect_load_error bad-colon.yml 'write the option without the colon: ms' 'on:
  - sub: "a"
    form: [moving-avg, :ms, 5, $payload-float]'
expect_load_error bad-op.yml 'must be quoted in YAML' 'on:
  - sub: "a"
    form: [>, $payload-float, 1]'
expect_load_error bad-thread-map.yml 'from/steps map form was removed' 'on:
  - sub: "a"
    thread:
      from: $payload-float
      steps:
        - [round, 1]'
expect_load_error bad-body.yml 'use do' 'on:
  - sub: "a"
    body: [publish!, "x"]'
expect_load_error bad-when-map.yml 'map form was removed' 'on:
  - sub: "a"
    when:
      test: [not, $replaying]
      then: [publish!, "x"]'
expect_load_error bad-unknown-form.yml 'unknown form: bogus' 'on:
  - sub: "a"
    form: [bogus, 1]'
expect_load_error bad-do-unknown-form.yml 'unknown form: bogus' 'on:
  - sub: "a"
    do: [bogus, 1]'
expect_load_error bad-vec.json '{"vector": [...]}' '[["on", "a", ["contains?", {"vec": [1, 2]}, "$payload"]]]'
expect_load_error bad-dollar.json 'unknown binding $nope' '[["on", "a", ["publish!", "x", "$nope"]]]'

option_yaml="$tmp/options.yml"
cat > "$option_yaml" <<'EOF'
on:
  - sub: "a"
    when: [">", [moving-avg, {ms: 5000}, $payload-float], 0]
    pipeline: [$payload-float, [moving-avg, {ms: 5000}], [publish!, "out", {dp: 1}]]
  - sub: "b"
    do: [dropout, {ms: 500, lost: [publish!, "lost"], found: [publish!, "found"]}]
EOF
"$bin" --validate "$option_yaml" > "$tmp/options.out" 2> "$tmp/options.err"
grep ': ok (2 rules)' "$tmp/options.out" >/dev/null
expect_load_error bad-option-duplicate.yml 'duplicate YAML option key' 'on:
  - sub: "a"
    do: [publish!, "x", {dp: 1, dp: 2}]'
expect_load_error bad-option-unknown.yml 'unknown YAML option key' 'on:
  - sub: "a"
    do: [publish!, "x", {bogus: 1}]'

# Every YAML example must also be valid YAML for a standard parser.
if command -v ruby >/dev/null 2>&1; then
    for yml in "$root"/examples/*.yml "$every_yaml" "$every_yaml_when" "$option_yaml"; do
        ruby -ryaml -e 'YAML.load_file(ARGV[0])' "$yml"
    done
else
    echo "note: ruby not found, skipping standard-YAML check of examples" >&2
fi

for bad in '(every :ms 0 (publish! "a" "b"))' '(every :ms 10)' '(every 10 (publish! "a" "b"))' '(every :ms 10 (bogus 1))' '(every :ms 10 :reentrant 1 (publish! "a" "b"))' '(on "t" (every :ms 10 (publish! "a" "b")))' '(every :ms 10 (reply!))'; do
    printf '%s\n' "$bad" > "$tmp/bad-every.edn"
    if "$bin" --validate "$tmp/bad-every.edn" > "$tmp/bad-every.out" 2> "$tmp/bad-every.err"; then
        echo "validate accepted bad every form: $bad" >&2
        exit 1
    fi
done

# Parse errors report line:col.
bad_pos="$tmp/bad-pos.edn"
printf '(on "a"\n  (publish! "b" "c")\n' > "$bad_pos"
if "$bin" --validate "$bad_pos" > "$tmp/pos.out" 2> "$tmp/pos.err"; then
    echo "truncated patchbay unexpectedly validated" >&2
    exit 1
fi
grep 'parse error: .* at 3:1 ' "$tmp/pos.err" >/dev/null

printf 'cli tests passed\n'
