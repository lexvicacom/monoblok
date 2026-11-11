description: The last-value cache for late joiners, live $STATS counters, and snapshots for warm restarts.

# LVC, stats and snapshots

{lede} Three pieces of server state sit beside the rules: a last-value cache that makes late-joining subscribers a non-event, live counters for every rule, and snapshots that carry both across a restart.

{{section docs/overview.md "`$LVC.*`: last-value-cache stream" level=2}}

### Why it matters

A dashboard that opens in the morning wants the current state of everything, immediately, with no "waiting for first reading" spinner. A logger that restarts wants to know where things stood. Subscribe to `$LVC.car.*.*.stable` and you receive the last known value for every matching subject, then the live stream. No JetStream, no external KV store: the broker remembers the last thing it saw.

```sh
# Publish a value, then walk away.
nats pub sensors.temp 23.5

# Later, subscribe.
nats sub '$LVC.sensors.temp'
# -> prints "23.5" immediately
```

Single-quote any subject starting with `$` so your shell does not try to expand it.

The cache pairs particularly well with conditioning rules. A rule's own publishes are cached too, so a subscriber to `$LVC.sensors.temp.stable` gets the most recent rule-produced value on connect, not the raw input. A dedupe rule is what makes that cached value useful: without `squelch`, the cache would hold whatever noisy reprint happened to land last.

### Reading the cache from a rule

Rules can read the cache with [`(last SUBJECT)`](forms.html#last-values), which is how one subject's rule sees another subject's value, and answer NATS requests from it with [`reply!`](forms.html#request-reply). The [HTTP adapter](clients.html#http-sse-adapter) exposes the same cache at `GET /latest/<subject>`.

```edn
(lvc ["temp.>"])

(on "ask.outside"
  (reply! (last "temp.outside")))
```

{{section docs/overview.md "`$STATS.*`: live counters" level=2}}

The per-rule counters are the quickest way to confirm a gate is doing what you expect: `emitted` climbing while `suppressed` stays at zero means the gate never closes.

{{section docs/overview.md "Snapshots" level=2}}

### What a snapshot holds

- Every last-value cache entry.
- Patchbay state: the last value inside each `squelch` and `deadband`, window rings, edge and `transition` state, counters, and in-progress bars.

Rule state is identified by the rule's position and filter plus the call site and subject of each slot. Time-window slots are re-armed on load; a slot whose deadline passed while the server was down fires immediately on startup. `every` timers are not stored; they are recreated when the patchbay loads.

The [systemd unit](running.html#systemd) writes a snapshot every 10 seconds and on stop, so a crash or reboot loses at most one snapshot interval of conditioning state.
