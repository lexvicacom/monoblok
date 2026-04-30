#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/demo.edn

subscribe 'demo.sensors.temp.stable'    stable.txt
subscribe 'demo.sensors.temp.delta'     delta.txt
subscribe 'demo.sensors.temp.smoothed'  smoothed.txt
subscribe 'demo.sensors.temp.avg5s'     avg5s.txt
subscribe 'demo.sensors.temp.alert'     alert.txt
subscribe 'demo.sensors.temp.ok'        ok.txt
subscribe 'demo.sensors.temp.overload'  overload.txt
subscribe 'demo.sensors.temp.spike'     spike.txt
subscribe 'demo.sensors.temp.count'     count.txt
subscribe 'demo.alerts'                 alerts.txt
settle 0.3

for v in 20.01 20.04 20.06 20.5 21.0 27.5 28.5 29.0 27.0 41.0 42.0 41.5 51.0 52.0 49.0 53.0; do
    pub demo.sensors.temp "$v"
    sleep 0.05
done

pub demo.log.kernel "kernel: alert! disk almost full"
pub demo.log.kernel "kernel: routine info"

settle 0.6

note "Drove demo.sensors.temp through a climb from 20 to 53, plus two demo.log.kernel lines. Each rule below is showcased by a different output stream (squelch, deadband, moving averages, transition edges, hold-off, rising-edge, and the log-to-alerts mirror)."
show "publishes"               _pubs.log
show "stable mirror"           stable.txt
show "deadband (>0.5 moves)"   delta.txt
show "moving-avg(10) smoothed" smoothed.txt
show "5s windowed avg"         avg5s.txt
show "transition: hot edge"    alert.txt
show "transition: cool edge"   ok.txt
show "hold-off overload"       overload.txt
show "rising-edge spike"       spike.txt
show "crossing count"          count.txt
show "log->alerts mirror"      alerts.txt
