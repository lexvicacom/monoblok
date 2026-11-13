#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/deidentify.edn

subscribe 'clean.>'  clean.txt
subscribe 'phi.held' held.txt
settle 0.3

pub ward.icu.p4471 '{"name":"Ada Example","mrn":"000-11-2222","age":47,"hr":72,"spo2":97}'
pub ward.icu.p9020 '{"name":"Bo Sample","mrn":"000-33-4444","age":81,"hr":90,"spo2":93}'
pub notes.icu "Comfortable overnight, obs stable"
pub notes.icu "Call family re MRN 000-11-2222"
pub notes.icu "Discharge review booked, born 3/4/1980"
pub notes.icu "MRN 4471, check chart"

settle 0.6

note \
    "Two patient frames and four free-text notes were published on ward.> and notes.>." \
    "The clean stream carries heart rate, SpO2, an age band and a ward average, with no name, record number or patient token. Notes were forwarded with record numbers and dates masked as [MRN] and [DATE]. The note with a bare number after the MRN label was held; only its subject was reported."
show "clean stream"  clean.txt
show "held notes"    held.txt
