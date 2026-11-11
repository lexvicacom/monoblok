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

# A YAML or JSON example and its EDN twin must produce identical soundcheck output.
rental_in='car.1.rpm|1010
car.1.rpm|1023
car.1.rpm|1190
car.1.coolant|88.1
car.1.coolant|88.4
car.1.coolant|90.3
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000
car.1.rpm|8000'
for ext in edn yml; do
    printf '%s\n' "$rental_in" | "$bin" --soundcheck --soundcheck-label --soundcheck-linger-ms 0 "examples/rental-car.$ext" > "$tmp/rental-$ext.out"
done
grep '^out|car.1.rpm.alert|' "$tmp/rental-edn.out" >/dev/null
grep '^out|car.1.coolant.stable|' "$tmp/rental-edn.out" >/dev/null
cmp "$tmp/rental-edn.out" "$tmp/rental-yml.out"

demo_in='demo.sensors.temp|20.04
demo.sensors.temp|20.06
demo.sensors.temp|29.5
demo.sensors.temp|55
demo.sensors.temp|20
demo.sensors.temp|45
demo.log.a|all quiet
demo.log.b|alert: door open'
for ext in edn yml json; do
    printf '%s\n' "$demo_in" | "$bin" --soundcheck --soundcheck-label --soundcheck-linger-ms 0 "examples/demo.$ext" > "$tmp/demo-$ext.out"
done
grep '^out|demo.alerts|' "$tmp/demo-edn.out" >/dev/null
grep '^out|demo.sensors.temp.alert|' "$tmp/demo-edn.out" >/dev/null
cmp "$tmp/demo-edn.out" "$tmp/demo-yml.out"
cmp "$tmp/demo-edn.out" "$tmp/demo-json.out"

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

# Two squelch calls in one rule keep separate state (state identity includes the call site).
site_patchbay="$tmp/site.edn"
cat > "$site_patchbay" <<'EOF'
(on "t.>" (do (publish! "out.a" (squelch (json-get "a" payload)))
              (publish! "out.b" (squelch (json-get "b" payload)))))
EOF
site_out="$(printf 't.x|{"a":1,"b":2}\nt.x|{"a":1,"b":2}\nt.x|{"a":1,"b":2}\n' | "$bin" --soundcheck "$site_patchbay")"
test "$(printf '%s\n' "$site_out" | grep -c '^out.a|1$')" -eq 1
test "$(printf '%s\n' "$site_out" | grep -c '^out.b|2$')" -eq 1

# on-silence still fires after another rule churns the scratch arena.
silence_patchbay="$tmp/silence.edn"
cat > "$silence_patchbay" <<'EOF'
(on "t.>" (on-silence :ms 50 (publish! "out.silent" "gone")))
(on "u.>" (-> payload (str-concat "a" "b" "c" "d" "e" "f") (str-concat "x") (publish! "out.u")))
EOF
silence_out="$(printf 't.x|hello\nu.x|1\nu.x|2\n' | "$bin" --soundcheck --soundcheck-linger-ms 300 "$silence_patchbay")"
printf '%s\n' "$silence_out" | grep '^out.silent|gone$' >/dev/null

# Threading into on-silence is rejected up front (its body would dangle).
threaded_silence="$tmp/threaded-silence.edn"
cat > "$threaded_silence" <<'EOF'
(on "t.>" (-> payload (on-silence :ms 50 (publish! "out.silent" "gone"))))
EOF
if printf 't.x|hello\n' | "$bin" --soundcheck "$threaded_silence" > "$tmp/ts.out" 2> "$tmp/ts.err"; then
    echo "threaded on-silence unexpectedly accepted" >&2
    exit 1
fi
grep 'cannot be a -> step' "$tmp/ts.err" >/dev/null

# Unknown forms are rejected even on branches the input never takes.
bogus_patchbay="$tmp/bogus.edn"
cat > "$bogus_patchbay" <<'EOF'
(on "t.>" (when (> payload-float 5) (bogus 1 2)))
EOF
if printf 't.x|1\n' | "$bin" --soundcheck "$bogus_patchbay" > "$tmp/bogus.out" 2> "$tmp/bogus.err"; then
    echo "unknown form unexpectedly accepted by soundcheck" >&2
    exit 1
fi
grep 'unknown form: bogus' "$tmp/bogus.err" >/dev/null

# A re-entrant rule funnels subjects onto one subject; the second rule keeps one
# average across them. Rows interleave as in the server: input, funnel row, then
# whatever its re-evaluation emits.
funnel_patchbay="$tmp/funnel.edn"
cat > "$funnel_patchbay" <<'EOF'
(on "sensors.*" :reentrant true (publish! "all.sensors" payload))
(on "all.sensors" (publish! "sensors.average" :dp 1 (moving-avg 4 payload-float)))
EOF
funnel_out="$(printf 'sensors.kitchen|20\nsensors.hall|22\nsensors.kitchen|21\nsensors.hall|23\n' | "$bin" --soundcheck "$funnel_patchbay")"
funnel_want='sensors.kitchen|20
all.sensors|20
sensors.average|20.0
sensors.hall|22
all.sensors|22
sensors.average|21.0
sensors.kitchen|21
all.sensors|21
sensors.average|21.0
sensors.hall|23
all.sensors|23
sensors.average|21.5'
test "$funnel_out" = "$funnel_want"

# Per-group funnel: the funnel subject is computed from a token.
group_patchbay="$tmp/group.edn"
cat > "$group_patchbay" <<'EOF'
(on "temp.*.*" :reentrant true (publish! (str-concat "site." (subject-token 1)) payload))
(on "site.*" (publish! (str-concat "avg." (subject-token 1)) :dp 1 (moving-avg 10 payload-float)))
EOF
group_out="$(printf 'temp.a.1|10\ntemp.b.1|40\ntemp.a.2|20\n' | "$bin" --soundcheck "$group_patchbay")"
group_want='temp.a.1|10
site.a|10
avg.a|10.0
temp.b.1|40
site.b|40
avg.b|40.0
temp.a.2|20
site.a|20
avg.a|15.0'
test "$group_out" = "$group_want"

# A rule that feeds itself stops at the depth cap (8 hops: 9 rows) and does not hang.
selfeed_patchbay="$tmp/selfeed.edn"
cat > "$selfeed_patchbay" <<'EOF'
(on "loop.*" :reentrant true (publish! "loop.b" payload))
EOF
selfeed_out="$(printf 'loop.a|1\n' | "$bin" --soundcheck "$selfeed_patchbay" 2> "$tmp/selfeed.err")"
test "$(printf '%s\n' "$selfeed_out" | grep -c '^loop.b|1$')" -eq 9
grep 'reentry depth cap reached' "$tmp/selfeed.err" >/dev/null

# Without :reentrant an emission is not fed back.
plain_patchbay="$tmp/plain-funnel.edn"
cat > "$plain_patchbay" <<'EOF'
(on "sensors.*" (publish! "all.sensors" payload))
(on "all.sensors" (publish! "sensors.average" (moving-avg 4 payload-float)))
EOF
plain_out="$(printf 'sensors.kitchen|20\n' | "$bin" --soundcheck "$plain_patchbay")"
test "$plain_out" = "sensors.kitchen|20
all.sensors|20"

# Clock-fired emissions of a re-entrant rule re-enter too.
tick_reentry="$tmp/tick-reentry.edn"
cat > "$tick_reentry" <<'EOF'
(every :ms 50 :reentrant true (publish! "tick.raw" "x"))
(on "tick.raw" (publish! "tick.seen" payload))
EOF
tick_out="$(printf '' | "$bin" --soundcheck --soundcheck-linger-ms 200 "$tick_reentry")"
printf '%s\n' "$tick_out" | grep '^tick.seen|x$' >/dev/null

# examples/bars.edn closes bars on a re-entrant rule; the alert rule consumes the closed bars.
bars_out="$({ i=0; while [ "$i" -lt 60 ]; do echo 'MARKET.AAPL|100'; i=$((i + 1)); done; i=0; while [ "$i" -lt 60 ]; do echo 'MARKET.AAPL|105'; i=$((i + 1)); done; } | "$bin" --soundcheck examples/bars.edn)"
printf '%s\n' "$bars_out" | grep '^MARKET.AAPL.bar.close|100$' >/dev/null
printf '%s\n' "$bars_out" | grep '^MARKET.AAPL.bar.close|105$' >/dev/null
printf '%s\n' "$bars_out" | grep '^alerts.market.AAPL|105$' >/dev/null

# A malformed :reentrant option fails like the server loader.
bad_option="$tmp/bad-option.edn"
printf '(on "x" :reentrant (publish! "y" payload))\n' > "$bad_option"
if printf 'x|1\n' | "$bin" --soundcheck "$bad_option" > /dev/null 2> "$tmp/bad-option.err"; then
    echo "soundcheck accepted a malformed :reentrant option" >&2
    exit 1
fi

# last reads what (lvc ...) covers: input rows and emitted rows, nil otherwise.
last_patchbay="$tmp/last.edn"
cat > "$last_patchbay" <<'EOF'
(lvc ["temp.*" "derived.>"])
(on "q" (publish! "ans.outside" (if (last "temp.outside") (last "temp.outside") "none")))
(on "q" (publish! "ans.other" (if (last "other.x") (last "other.x") "none")))
(on "q" (publish! "ans.hot" (if (and (last "temp.outside") (> (last "temp.outside") 30)) "hot" "mild")))
(on "temp.*" (publish! "derived.copy" payload))
(on "r" (publish! "ans.derived" (last "derived.copy")))
(on "temp.*" (publish! "ans.now" (last subject)))
EOF
last_out="$(printf 'q|go\nother.x|7\ntemp.outside|31.5\nq|go\nr|go\n' | "$bin" --soundcheck "$last_patchbay")"
printf '%s\n' "$last_out" | grep '^ans.outside|none$' >/dev/null
printf '%s\n' "$last_out" | grep '^ans.outside|31.5$' >/dev/null
printf '%s\n' "$last_out" | grep '^ans.other|none$' >/dev/null
printf '%s\n' "$last_out" | grep '^ans.hot|hot$' >/dev/null
printf '%s\n' "$last_out" | grep '^ans.derived|31.5$' >/dev/null
printf '%s\n' "$last_out" | grep '^ans.now|31.5$' >/dev/null

# Without (lvc ...) soundcheck has nothing for last to read.
nolvc_patchbay="$tmp/nolvc.edn"
cat > "$nolvc_patchbay" <<'EOF'
(on "q" (publish! "ans" (if (last "temp.outside") "seen" "none")))
EOF
nolvc_out="$(printf 'temp.outside|31\nq|go\n' | "$bin" --soundcheck "$nolvc_patchbay")"
printf '%s\n' "$nolvc_out" | grep '^ans|none$' >/dev/null

# hysteresis and ewma through soundcheck.
smooth_patchbay="$tmp/smooth.edn"
cat > "$smooth_patchbay" <<'EOF'
(on "t" (publish! "t.ewma" (ewma 0.5 payload-float)))
(on "t" (publish! "t.alarm" (hysteresis 20 25 payload-float)))
EOF
smooth_out="$(printf 't|10\nt|30\nt|22\nt|18\n' | "$bin" --soundcheck "$smooth_patchbay")"
printf '%s\n' "$smooth_out" | grep '^t.ewma|20$' >/dev/null
printf '%s\n' "$smooth_out" | grep '^t.ewma|21$' >/dev/null
printf '%s\n' "$smooth_out" | grep '^t.ewma|19.5$' >/dev/null
test "$(printf '%s\n' "$smooth_out" | grep -c '^t.alarm|true$')" -eq 2
test "$(printf '%s\n' "$smooth_out" | grep -c '^t.alarm|false$')" -eq 2

# every fires during linger, once per interval, with empty subject/payload and nil reply-to.
every_patchbay="$tmp/every.edn"
cat > "$every_patchbay" <<'EOF'
(lvc "temp.*")
(every :ms 100 (publish! "tick" (str-concat "[" subject payload (if reply-to "r" "-") (if (last "temp.a") (last "temp.a") "none") "]")))
(on "temp.*" (publish! "echo" payload))
EOF
every_out="$(printf 'temp.a|7\n' | "$bin" --soundcheck --soundcheck-linger-ms 450 "$every_patchbay")"
test "$(printf '%s\n' "$every_out" | grep -c '^tick|\[-7\]$')" -ge 3
test "$(printf '%s\n' "$every_out" | grep -c '^tick|\[-7\]$')" -le 5
printf '%s\n' "$every_out" | grep '^echo|7$' >/dev/null

# No linger, no fire: every is only scheduled, never run at load or for inputs.
every_quiet="$(printf 'temp.a|7\n' | "$bin" --soundcheck --soundcheck-linger-ms 0 "$every_patchbay")"
if printf '%s\n' "$every_quiet" | grep '^tick|' >/dev/null; then
    echo "soundcheck every fired without linger" >&2
    exit 1
fi

# A long interval does not fire inside a short linger.
every_long="$tmp/every-long.edn"
printf '(every :ms 60000 (publish! "tick" "x"))\n' > "$every_long"
every_long_out="$(printf '' | "$bin" --soundcheck --soundcheck-linger-ms 200 "$every_long")"
test -z "$every_long_out"

# Stateful forms inside every share the empty key; on-silence slots armed there see an empty subject.
every_state="$tmp/every-state.edn"
cat > "$every_state" <<'EOF'
(every :ms 100 (publish! "n" (moving-sum 10000 1)))
(every :ms 100 (on-silence :ms 30 (publish! "quiet" (str-concat "[" subject "]"))))
EOF
every_state_out="$(printf '' | "$bin" --soundcheck --soundcheck-linger-ms 380 "$every_state")"
printf '%s\n' "$every_state_out" | grep '^n|1$' >/dev/null
printf '%s\n' "$every_state_out" | grep '^n|2$' >/dev/null
printf '%s\n' "$every_state_out" | grep '^quiet|\[\]$' >/dev/null

printf 'soundcheck passed\n'
