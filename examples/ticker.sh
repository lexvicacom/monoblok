#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/ticker.edn

subscribe 'MARKET.AAPL'         raw.txt
subscribe 'MARKET.AAPL.stable'  stable.txt
subscribe 'alerts.market.>'     alerts.txt
settle 0.3

for v in 182.3401 182.3402 182.3404 182.5001 182.5002 184.0001 184.0009 184.0011; do
    pub MARKET.AAPL "$v"
done

settle 0.5

note \
    "Streamed eight AAPL ticks." \
    "The stable mirror gets 3dp values with duplicates squelched." \
    "A >= 1.0 jump between consecutive ticks fans out an alert under alerts.market.AAPL."
show "publishes"        _pubs.log
show "raw ticks"        raw.txt
show "stable mirror"    stable.txt
show "jump alerts"      alerts.txt
