description: The last-value cache for late joiners, live $STATS counters, and snapshots for warm restarts.

# LVC, stats and snapshots

{lede} The last-value cache supplies recent values to new subscribers, live counters report rule activity, and snapshots preserve cached values and patchbay state across restarts.

{{section docs/overview.md "`$LVC.*`: last-value-cache stream" level=2}}

### Why it matters

Subscribe to `$LVC.car.*.*.stable` to receive the last cached value for each matching subject, followed by live updates. This lets a dashboard display recent readings even when no new messages are arriving.

```sh
# Publish a value before subscribing.
nats pub sensors.temp 23.5

# Later, subscribe.
nats sub '$LVC.sensors.temp'
# -> prints "23.5" immediately
```

Single-quote any subject starting with `$` so your shell does not try to expand it.

Rule output is cached when its subject matches an LVC filter. A subscriber to `$LVC.sensors.temp.stable` therefore receives the latest processed value on connection.

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
