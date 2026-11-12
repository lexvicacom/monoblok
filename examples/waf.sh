#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/waf.edn

subscribe 'clean.event.*'   clean-event.txt
subscribe 'clean.reading.*' clean-reading.txt
subscribe 'waf.blocked'     blocked.txt
subscribe 'waf.flood.>'     flood.txt
settle 0.3

# Payload scan: two ordinary events and two that carry bad fragments.
pub in.event.orders   '{"note":"leave at the door"}'
pub in.event.orders   '{"note":"<script>alert(1)</script>"}'
pub in.event.signup   '{"name":"Ada"}'
pub in.event.signup   '{"name":"x; DROP TABLE users"}'

# Outlier gate: steady readings around 400 with two wild ones.
for v in 412 398 405 391 388 9000 402 395 64000 399; do
    pub in.reading.pump7 "$v"
done

# Flood detection: one subject publishing in a burst, another publishing once.
i=0
while [ $i -lt 12 ]; do
    pub in.event.chatty "tick $i"
    i=$((i+1))
done
pub in.event.quiet "hello"

settle 0.6

note \
    "Publishers sent events and readings under in.>." \
    "Events with bad fragments were reported on waf.blocked and not forwarded, readings above the rolling p99 were held back, and the subject that published twelve messages in a burst got one block message."
show "forwarded events"    clean-event.txt
show "blocked subjects"    blocked.txt
show "forwarded readings"  clean-reading.txt
show "flood verdicts"      flood.txt
