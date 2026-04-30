#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../common.sh"

if ! command -v node >/dev/null; then
    echo "node is required to run mock_producer.js" >&2
    exit 1
fi

DURATION="${DURATION:-5}"

start_daemon examples/json-massive/massive.edn

subscribe 'T.*'           trades.txt
subscribe 'T.*.p'         trade_prices.txt
subscribe 'T.*.p.stable'  trade_stable.txt
subscribe 'alerts.>'      alerts.txt
settle 0.3

NATS_PORT="$PORT" node "$ROOT/examples/json-massive/mock_producer.js" \
    >"$TMP/producer.log" 2>&1 &
PRODUCER_PID=$!
EXTRA_PIDS+=("$PRODUCER_PID")

echo "running mock_producer for ${DURATION}s (DURATION=$DURATION)..."
sleep "$DURATION"

{ kill "$PRODUCER_PID" 2>/dev/null && wait "$PRODUCER_PID"; } 2>/dev/null || true
settle 0.4

note "A Node mock_producer drove a high-rate Massive-shape feed into monoblok for ${DURATION}s, mixing stocks, options, crypto, and forex. The tables show the stock-trade slice plus the alerts.> fan-in across all asset classes."
show "raw stock trades (last 8)"      trades.txt        8
show "demuxed prices (last 8)"        trade_prices.txt  8
show "stable mirror (last 8)"         trade_stable.txt  8
show "big-move alerts (last 12)"      alerts.txt        12
