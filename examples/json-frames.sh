#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/json-frames.edn

subscribe 'devices.kitchen'                raw.txt
subscribe 'devices.kitchen.temp'           temp.txt
subscribe 'devices.kitchen.hum'            hum.txt
subscribe 'devices.kitchen.batt'           batt.txt
subscribe 'devices.kitchen.temp.stable'    temp_stable.txt
subscribe 'devices.kitchen.hum.stable'     hum_stable.txt
subscribe 'devices.kitchen.battery'        battery.txt
settle 0.3

pub devices.kitchen '{"temp":21.04,"hum":62,"batt":3.71}'
pub devices.kitchen '{"temp":21.06,"hum":62,"batt":3.70}'
pub devices.kitchen '{"temp":21.30,"hum":63,"batt":3.65}'
pub devices.kitchen '{"temp":21.31,"hum":63,"batt":3.64}'
pub devices.kitchen '{"temp":22.00,"hum":70,"batt":3.40}'
pub devices.kitchen 'not even json'
pub devices.kitchen '{"temp":22.01}'

settle 0.5

note "Six JSON frames and one garbage payload were published to devices.kitchen. json-demux fans each named field out to devices.kitchen.<field>, downstream rules round and deadband those scalars into stable mirrors, and the malformed/missing-field cases self-skip without erroring."
show "publishes"           _pubs.log
show "raw frames"          raw.txt
show "demuxed temp"        temp.txt
show "demuxed hum"         hum.txt
show "demuxed batt"        batt.txt
show "stable temp mirror"  temp_stable.txt
show "stable hum mirror"   hum_stable.txt
show "battery (json-get)"  battery.txt
