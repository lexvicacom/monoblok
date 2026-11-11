#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

start_daemon examples/ask.edn

subscribe 'temp.difference' difference.txt
settle 0.3

# req SUBJECT: send a request and append the reply (or "no reply") to replies.txt.
req() {
    local reply
    # The reply body is the only thing nats prints on stdout (blank lines aside).
    reply=$(nats --no-context -s "$URL" req --timeout 1s "$1" "" 2>/dev/null | awk 'NF { print; exit }' || true)
    echo "$1 -> ${reply:-no reply}" >>"$TMP/replies.txt"
}

# Phase 1: only temp.inside has a value. The join has no outside reading
# yet, so (last "temp.outside") is nil and nothing is published. Asking for
# the outside temperature gets no answer either (reply! is a no-op on nil).
pub temp.inside "21.5"
settle 0.2
req ask.outside

# Phase 2: outside arrives. The next inside reading joins with it.
pub temp.outside "5.0"
pub temp.inside "21.5"
settle 0.3
req ask.outside
req ask.difference

# Phase 3: outside moves; the join and the answers follow.
pub temp.outside "7.0"
pub temp.inside "21.0"
settle 0.3
req ask.difference

note \
    "temp.inside arrived first, with no temp.outside to join: (last ...) was nil, the thread stopped, and the first request got no reply." \
    "Once temp.outside existed, each temp.inside reading published temp.difference (inside minus outside), and requests to ask.outside and ask.difference were answered from the last-value cache with reply!."
show "publishes"                 _pubs.log
show "temp.difference"           difference.txt
show "request -> reply"          replies.txt
