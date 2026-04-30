#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/sensors.edn

subscribe 'sensors.temp'        raw.txt
subscribe 'sensors.temp.stable' stable.txt
settle 0.3

for v in 42.01 42.04 42.08 42.12 43.00 43.00 43.01; do
    pub sensors.temp "$v"
done

settle 0.4

note "Seven jittery temperature samples were published. The patchbay rounds to 1dp and squelches duplicates, so the stable mirror sees only distinct rounded values."
show "publishes"       _pubs.log
show "raw publishes"   raw.txt
show "stable mirror"   stable.txt
