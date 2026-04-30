#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/rental-car.edn

subscribe 'car.42.rpm.stable'     rpm.txt
subscribe 'car.42.rpm.alert'      alert.txt
subscribe 'car.42.coolant.stable' coolant.txt
settle 0.3

for v in 1010 1023 1041 1078 1135 1190; do pub car.42.rpm "$v"; done
for v in 88.1 88.4 88.7 89.5 90.2 90.3; do pub car.42.coolant "$v"; done

i=0
while [ $i -lt 30 ]; do pub car.42.rpm "8000"; i=$((i+1)); done

settle 0.6

note \
    "Drove small RPM and coolant moves first (RPM quantized to 50rpm buckets and squelched, coolant deadbanded by 1.0)." \
    "Then floored the engine: 30 publishes at 8000rpm." \
    "The 5s hold-off collapses that spam into exactly one over-rev alert."
show "publishes (last 12)"                     _pubs.log 12
show "rpm quantized+squelched (50rpm buckets)" rpm.txt
show "over-rev alert (hold-off 5s)"            alert.txt
show "coolant stable (deadband 1.0)"           coolant.txt
