#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/lvc.edn

# Phase 1: publish a few values with no subscriber attached.
pub room.kitchen.temp "21.3"
pub room.kitchen.temp "21.5"
pub room.kitchen.temp "21.7"
pub room.bedroom.temp "19.8"
pub room.office.temp  "23.1"
settle 0.3

# Phase 2: a late joiner subscribes to $LVC.>. The first three messages
# they receive are cache replay (last value per subject); the next two
# are live updates from publishes that happened after the subscribe.
subscribe '$LVC.>' lvc.txt
settle 0.3

pub room.kitchen.temp "21.9"
pub room.office.temp  "23.4"

settle 0.4

note \
    "Five values were published to room.* with no subscribers attached." \
    "A late joiner then subscribed to \$LVC.> and immediately received the cached last value per subject (the first three rows below, sub-millisecond timestamps)." \
    "Two further publishes arrived live afterwards on \$LVC.<subject>."
show "publishes"              _pubs.log
show "late joiner on \$LVC.>" lvc.txt
