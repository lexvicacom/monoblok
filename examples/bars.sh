#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/bars.edn

subscribe 'MARKET.AAPL'             raw.txt
subscribe 'MARKET.AAPL.bar.open'    bar_open.txt
subscribe 'MARKET.AAPL.bar.high'    bar_high.txt
subscribe 'MARKET.AAPL.bar.low'     bar_low.txt
subscribe 'MARKET.AAPL.bar.close'   bar_close.txt
subscribe 'alerts.market.>'         alerts.txt
settle 0.3

for i in $(seq 1 130); do
    price=$(awk -v i=$i 'BEGIN{
        base = (i <= 60) ? 180 + i*0.05 : 175 + (i-60)*0.05
        printf "%.2f", base
    }')
    pub MARKET.AAPL "$price"
done
settle 0.5

note \
    "Streamed 130 AAPL ticks." \
    "Bars close every 60 ticks, so two complete bars are emitted and a third is in progress." \
    "A >= 1.0 close-to-close move triggers an alert."
show "publishes (last 8)"  _pubs.log 8
show "raw ticks (last 8)"  raw.txt 8
show "bar opens"           bar_open.txt
show "bar highs"           bar_high.txt
show "bar lows"            bar_low.txt
show "bar closes"          bar_close.txt
show "jump alerts"         alerts.txt
