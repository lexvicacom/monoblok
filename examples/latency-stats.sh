#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/latency-stats.edn

subscribe 'rpc.latency.p50'    p50.txt
subscribe 'rpc.latency.p95'    p95.txt
subscribe 'rpc.latency.p99'    p99.txt
subscribe 'rpc.latency.stddev' stddev.txt
settle 0.3

# Phase 1: 50 well-behaved samples around 20ms, light jitter.
i=0
while [ $i -lt 50 ]; do
    v=$(awk -v i=$i 'BEGIN { srand(i+1); printf "%.2f", 20 + (rand()*4 - 2) }')
    pub rpc.latency "$v"
    i=$((i+1))
done

# Phase 2: a tail-latency burst (10 slow requests, ~250ms) on top of
# the same baseline. The window is still 50 samples, so by the end of
# this phase the slow tail is fully represented in p99 and stddev.
i=0
while [ $i -lt 10 ]; do
    v=$(awk -v i=$i 'BEGIN { srand(i+100); printf "%.2f", 250 + (rand()*40 - 20) }')
    pub rpc.latency "$v"
    i=$((i+1))
done

# Phase 3: another 50 calm samples, watch the burst age out of the
# window and the stats recover.
i=0
while [ $i -lt 50 ]; do
    v=$(awk -v i=$i 'BEGIN { srand(i+200); printf "%.2f", 20 + (rand()*4 - 2) }')
    pub rpc.latency "$v"
    i=$((i+1))
done

settle 0.6

note \
    "Drove a synthetic latency stream through three phases: calm baseline (~20ms), a 10-sample tail-latency burst (~250ms), then another calm baseline." \
    "Four rules share the same 50-sample window, each emitting one statistic. p50 barely moves, p95 and p99 spike when the burst lands, stddev tracks the widening spread, and all three settle once the burst rolls out of the window."
show "p50 (last 12)"     p50.txt    12
show "p95 (last 12)"     p95.txt    12
show "p99 (last 12)"     p99.txt    12
show "stddev (last 12)"  stddev.txt 12
