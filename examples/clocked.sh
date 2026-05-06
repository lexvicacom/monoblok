#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/clocked.edn

subscribe 'clocked.sensor.temp'           raw.txt
subscribe 'clocked.sensor.temp.stale'     stale.txt
subscribe 'clocked.sensor.temp.debounced' debounced.txt
subscribe 'clocked.sensor.temp.sampled'   sampled.txt
subscribe 'clocked.sensor.temp.avg5s'     avg5s.txt
subscribe 'clocked.sensor.temp.rate5s'    rate5s.txt
settle 0.3

for v in 20 21 22 23 24; do
    pub clocked.sensor.temp "$v"
    settle 0.1
done

settle 1.2
pub clocked.sensor.temp 30
settle 6.0

note \
    "Published a short burst, then one later update, then went quiet." \
    "debounce! emits the final value after 250ms of quiet." \
    "sample! emits the latest value once per second." \
    "aggregate! emits 5-second average/rate values from the clock." \
    "on-silence marks the subject stale after 5 seconds without input."
show "publishes"          _pubs.log
show "raw input"          raw.txt
show "stale flag"         stale.txt
show "debounced latest"   debounced.txt
show "sampled latest"     sampled.txt
show "5s average"         avg5s.txt
show "5s event rate"      rate5s.txt
