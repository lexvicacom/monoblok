# monoblok public demo

A shared monoblok instance for poking at patchbay rules without
running your own server. Loaded from [`examples/demo.edn`](../examples/demo.edn).

**Shared, public, no auth.** Anyone can publish, anyone can
subscribe to anything. Don't send secrets. Don't rely on `$LVC`
retention.

> **Small VPS.** This is a single low-spec box with no rate limiting
> and no backpressure beyond the OS. A tight publish loop or
> hundreds of concurrent subscribers will knock it over. If it's
> dead when you try it, wait a bit and try again, or spin up your
> own: `./zig-out/bin/monoblok --port 4222 --patchbay examples/demo.edn`.

## Quick start

You need the [`nats` CLI](https://github.com/nats-io/natscli). Save
the demo server as a context once, then select it and forget about
the URL:

```
nats context save monoblok-demo --server nats://demo.monoblok.host:4222
nats context select monoblok-demo
```

Now `nats pub` and `nats sub` go to the demo server by default.

```
# In one terminal: watch everything the rules derive.
nats sub 'demo.>'

# In another: drive some input.
nats pubdemo.sensors.temp 25.3
nats pubdemo.sensors.temp 25.34
nats pubdemo.sensors.temp 25.38
```

Subject names starting with `$` (like `$LVC.*`) need single-quoting
so the shell doesn't try to expand them as a variable.

You'll see derived publishes on `demo.sensors.temp.stable`,
`.delta`, `.smoothed`, `.delta-abs` etc., each produced by a
different patchbay rule.

## Subject map

All visitor input goes under `demo.>`. Derived streams reuse the
input subject plus a suffix.

| Publish here                         | Watch here                                  | What the rule does                                        |
|--------------------------------------|---------------------------------------------|-----------------------------------------------------------|
| `demo.sensors.<name>` (a number)     | `demo.sensors.<name>.stable`                | `round 1` then `squelch` (emit when rounded value moves)  |
|                                      | `demo.sensors.<name>.delta`                 | `deadband 0.5` (suppress moves smaller than 0.5)          |
|                                      | `demo.sensors.<name>.smoothed`              | `moving-avg 10` then `deadband 1.0`               |
|                                      | `demo.sensors.<name>.delta-abs`             | per-tick numeric `delta` (0 on first sight)               |
|                                      | `demo.sensors.<name>.alert` / `.ok`         | `transition` across 28.0: "hot" rising, "cool" falling    |
|                                      | `demo.sensors.<name>.overload`              | `hold-off 2000` while `> 40` (one emit per 2s)            |
|                                      | `demo.sensors.<name>.spike`                 | `rising-edge` across 50.0                                 |
|                                      | `demo.sensors.<name>.count`                 | `count` of false-to-true crossings above 50.0             |
| `demo.log.<name>` (any text)         | `demo.alerts`                               | mirror if payload contains `alert`, prefixed with source  |

State (`squelch` last values, `moving-avg` rings, `transition`
previous state) is **per rule, per subject**, for the server's
lifetime. Two people both publishing to `demo.sensors.temp` share
the same state (that's a feature of the demo, not a bug).

## `$LVC.*` (last-value cache)

The demo patchbay opts `demo.>` into last-value caching with
`(lvc ["demo.>"])`. Subscribing to `$LVC.<subject>` gets you that cached
value immediately, then live updates, even if you subscribed long after
the publisher disconnected. No JetStream, no persistence, just "what was
the most recent value."

```
# Replay the current value of a single subject, then stream updates.
nats sub '$LVC.demo.sensors.temp'

# Snapshot-then-stream across a wildcard: every matching subject's
# cached value is delivered on subscribe.
nats sub '$LVC.demo.>'
```

Useful for late joiners: a dashboard that connects after an event
still sees the right state. `$LVC.*` is read-only; publishing to it
is rejected. The cache lives in memory only (a server restart
wipes it).

## `$STATS.*` (live counters)

Every minute the server emits cumulative totals on `$STATS.>`:
global publishes, per-rule emit/suppress counts, bridge
published/dropped (if a bridge is configured). Handy for confirming
a gate actually fired.

```
nats sub '$STATS.>'
```

`$STATS.*` is also read-only and intentionally excluded from the
LVC (a stale counter snapshot isn't useful).

## Walkthroughs

Assuming you've selected the `monoblok-demo` context, these commands
go straight to the public server. Each example assumes
`nats sub 'demo.>'` is running in another terminal. Payloads are
plain ASCII numbers.

### Squelch (value dedup)

```
nats pubdemo.sensors.temp 42.01   # -> .stable emits "42.0"
nats pubdemo.sensors.temp 42.04   # -> rounds to 42.0, same, suppressed
nats pubdemo.sensors.temp 42.08   # -> rounds to 42.1, emits
nats pubdemo.sensors.temp 42.12   # -> rounds to 42.1, suppressed
```

### Hold-off (time dedup)

```
nats pubdemo.sensors.temp 45      # -> .overload emits (first sight)
nats pubdemo.sensors.temp 46      # -> within 2s, suppressed
nats pubdemo.sensors.temp 47      # -> still within 2s, suppressed
# wait 2s
nats pubdemo.sensors.temp 48      # -> .overload emits
```

### Transition (both edges, one rule)

```
nats pubdemo.sensors.temp 20      # first sight, no edge
nats pubdemo.sensors.temp 30      # crossed 28 -> .alert "hot"
nats pubdemo.sensors.temp 31      # still above, nothing
nats pubdemo.sensors.temp 25      # crossed back -> .ok "cool"
```

### Counting threshold crossings

```
nats pubdemo.sensors.temp 10      # below 50, no spike, count unchanged
nats pubdemo.sensors.temp 60      # crosses 50 -> .spike, .count "1"
nats pubdemo.sensors.temp 70      # still above, no edge, count unchanged
nats pubdemo.sensors.temp 40      # drops below, no edge
nats pubdemo.sensors.temp 55      # crosses 50 again -> .spike, .count "2"
```

### Alerts from logs

```
nats pubdemo.log.db "heartbeat ok"        # nothing
nats pubdemo.log.db "replication alert!"  # -> demo.alerts "[db] replication alert!"
```

### Late-joining with `$LVC`

```
# Someone else publishes, you weren't listening.
nats pubdemo.sensors.temp 23.5

# Subscribe later. Normal SUB would see nothing until the next publish.
# $LVC gives you the last value immediately.
nats sub '$LVC.demo.sensors.temp'   # -> prints "23.5" on subscribe
```

### Bridge
```
(bridge
   :servers  ("nats://127.0.0.1:4223")
   :name     "monoblok-prod-1"
   :export   ("demo.sensors.*.spike" "demo.alerts.>"))
```

You can connect to `demo.monoblok.host:4223` which is a real NATS to observe the above being relayed.

```
nats -s 127.0.0.1:4223 sub "demo.sensors.*.spike"
```

## What it can't do

- **No ticker.** The server doesn't generate input on its own. A
  read-only visitor sees nothing on `demo.>` until someone
  publishes.
- **No auth.** Treat it as a whiteboard, not storage.
- **No JetStream, queues, headers, request/reply.** Core NATS only
  (`PUB`, `SUB`, `UNSUB`, `PING`/`PONG`, `INFO`, `-ERR`).

For the full primitive reference see [patchbay.md](./patchbay.md).
