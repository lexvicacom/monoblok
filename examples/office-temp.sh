#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/office-temp.edn

subscribe 'temp.f1.kitchen.stable' stable.txt
subscribe 'temp.f1.kitchen.alert'  alert.txt
subscribe 'temp.f1.kitchen.ok'     ok.txt
subscribe 'temp.f1.kitchen.count'  count.txt
settle 0.3

i=0
while [ $i -lt 60 ]; do pub temp.f1.kitchen "21.0"; i=$((i+1)); done
i=0
while [ $i -lt 60 ]; do pub temp.f1.kitchen "30.0"; i=$((i+1)); done
i=0
while [ $i -lt 60 ]; do pub temp.f1.kitchen "21.0"; i=$((i+1)); done
i=0
while [ $i -lt 60 ]; do pub temp.f1.kitchen "30.0"; i=$((i+1)); done

settle 0.6

note \
    "Drove the kitchen sensor through two cool/hot cycles of 60 samples each." \
    "The 60-tick moving average crosses 28.0 twice, so we get two 'hot' edges, two 'cool' edges, and a crossing count that reaches 2."
show "publishes (last 8)"     _pubs.log 8
show "stable mirror (last 6)" stable.txt 6
show "hot edge"             alert.txt
show "cool edge"             ok.txt
show "crossing count"       count.txt
