description: Sensor and telemetry patchbays: office temperature alerts, rental car telemetry, JSON gateways, joins, latency stats and clock forms.

# Sensors and telemetry

{lede} Patchbays for readings that arrive often and mostly say nothing new: room sensors, vehicle telemetry, JSON gateways and service latency samples.

## Office temperature

{tag:Intermediate} Three rules over `temp.<floor>.<room>`: a stable mirror for dashboards, a sustained-heat alert with an all-clear, and a tally of heat events.

{{include examples/office-temp.edn}}

What to notice:

- **`deadband` instead of `squelch`.** `squelch` passes any change; [`deadband 0.3`](forms.html#idempotent-filters) passes only when the value has moved 0.3 from the last one it let through, so a reading wobbling between 21.0 and 21.1 stays quiet.
- **`transition` covers both edges.** One rule, one predicate: the first branch runs once when the condition becomes true, the second once when it becomes false. Nothing is published in between.
- **`hysteresis` stops chatter.** A single 28°C threshold flips back and forth when the average hovers around it. [`(hysteresis 26.0 28.0 X)`](forms.html#hysteresis) turns on at 28 and off only at 26.
- **Call-site state.** The second and third rules both contain `(moving-avg 60 payload-float)`. Each is its own call site with its own ring, so they do not interfere.

```soundcheck examples/office-temp.edn
temp.2.kitchen|21.04
temp.2.kitchen|21.11
temp.2.kitchen|21.42
```

Run `examples/office-temp.sh` to drive enough samples to trip the alert and the all-clear.

## Rental car telemetry

{tag:Intermediate} The worked example from the [introductory blog post](https://alexjreid.dev/posts/monoblok/): catching over-revs at Peter's Porsche Rentals.

Peter rents out a dozen Porsche 911s and wants to record sustained high engine speeds while each car is on hire.

A script on a Raspberry Pi polls a Bluetooth OBD2 dongle for RPM, coolant temperature and other readings, publishing each to `car.<vin>.<pid>`. monoblok runs on the Pi and forwards processed subjects over a 5G connection to the reporting system.

What Peter wants:

- A per-car stream of RPM, coolant temperature and speed for the fleet dashboard.
- An alert when the moving average of engine speed exceeds 7500rpm.
- The latest cached readings for each car when the dashboard connects.

RPM updates many times a second and wobbles constantly at a steady throttle. Coolant barely moves once the engine is warm. Publishing all of this unconditioned over a metered 5G connection is wasteful.

{{include examples/rental-car.edn}}

```soundcheck examples/rental-car.edn
car.wp0ab.rpm|3012
car.wp0ab.rpm|3020
car.wp0ab.rpm|3049
car.wp0ab.rpm|3110
car.wp0ab.coolant|89.6
car.wp0ab.coolant|90.2
car.wp0ab.coolant|91.4
```

[`quantize 50`](forms.html#idempotent-filters) snaps RPM to 50rpm buckets, so the uplink only carries RPM when it moves into a new bucket, and coolant when it shifts by a degree. The coolant rule uses a deadband to suppress changes smaller than one degree.

The over-rev rule tests a 20-sample moving average against 7500rpm, reducing sensitivity to brief spikes. [`hold-off 5000`](forms.html#time-based-gates) then lets one alert through every five seconds, so a long stretch of redline does not publish once per incoming sample. State is per subject, so each car has its own ring buffer and its own hold-off timer.

To get a single alert and a single all-clear instead of a repeating alert, swap the last rule for a `transition`, as the [office temperature](#office-temperature) example does:

```edn
(on "car.*.rpm"
  (transition (> (moving-avg 20 payload-float) 7500.0)
    (publish! (subject-append "alert") payload)
    (publish! (subject-append "ok") payload)))
```

Peter's backend subscribes to `car.*.*.stable` and `car.*.*.alert` and gets a tidy, low-volume feed it can log, graph or react to without doing its own conditioning. Add `(lvc ["car.>"])` and a dashboard subscribing to `$LVC.car.*.*.stable` receives the last known value for every PID on every car without waiting for the next change. Add an [`export`](bridge.html#export) form listing `car.*.*.stable` and `car.*.*.alert` and only those subjects cross the 5G link to the office NATS cluster.

Local subscribers can react to the same alert, for example by triggering a dashcam over the local network. An office notification service can subscribe through the export. Neither needs changes to the patchbay rules.

### The same rules in YAML

{{include examples/rental-car.yml}}

`pipeline` is the `->` pipeline with the start value first, and `when` beside it is the rule-level guard. Bound values carry a `$`, and operators that are YAML indicators (`">"`) are quoted. See [YAML sugar files](patchbay.html#yaml-sugar-files).

## JSON frames from a gateway

{tag:Intermediate} Most off-the-shelf sensors and gateways emit JSON frames, not bare scalars. The pattern is to break the frame out at the head, then treat every field as a normal scalar stream.

{{include examples/json-frames.edn}}

```soundcheck examples/json-frames.edn
devices.kitchen|{"temp":12.52,"hum":80.2,"batt":3.7}
devices.kitchen|{"temp":12.58,"hum":80.4,"batt":3.7}
devices.kitchen|{"temp":13.1,"hum":81,"batt":3.6}
```

The first rule is marked `:reentrant true`, so the subjects it emits (`devices.kitchen.temp` and friends) are matched against the rules below it. Without that option they would reach subscribers but not the cleanup rules. The second frame shows the gates working: temperature moved 0.06 and humidity rounds to the same 80, so neither `.stable` subject repeats.

[`json-get`](forms.html#json-payloads) is the inline alternative when you want one field and no fan-out. Both forms accept dotted paths up to four levels deep, such as `"sensor.thermal.temp"`.

## Joins and request-reply

{tag:Intermediate} State is per subject, so a rule handling `temp.inside` cannot see `temp.outside` directly. [`(last SUBJECT)`](forms.html#last-values) reads another subject's value from the last-value cache, which makes a join; [`reply!`](forms.html#request-reply) answers NATS requests from the same cache.

{{include examples/ask.edn}}

```soundcheck examples/ask.edn
temp.inside|21.5
temp.outside|18
temp.inside|21.5
temp.inside|22
```

The first inside reading publishes nothing, because `temp.outside` has no value yet and a thread stops on nil. After that, each inside reading publishes the difference. Request-reply needs a real client, since `reply-to` is always nil under `--soundcheck`:

```sh
monoblok examples/ask.edn
nats pub temp.outside 18
nats req ask.outside ""
```

## Latency statistics

{tag:Intermediate} Five rules read one stream of per-request latency samples and each publishes a different statistic, so dashboards subscribe to whichever shape they care about without every consumer re-implementing the maths.

{{include examples/latency-stats.edn}}

Each rule has its own 50-sample window (call-site state again). Swap any `50` for `:ms 30000` to get a 30-second sliding window. [`ewma`](forms.html#exponential-average) is the cheap trend line: one number of state instead of a ring. See [Rate, percentiles, distribution shape](forms.html#rate-percentiles-distribution-shape).

## Clock forms and a funnel

{tag:Advanced} Most forms run only while handling a message. The clock forms keep a deadline, so they can publish after time passes even if the feed goes quiet. This file shows each of them, a top-level `every` heartbeat, and the funnel pattern for rolling several subjects into one piece of state.

{{include examples/clocked.edn}}

| rule | form | behaviour |
|---|---|---|
| stale detection | [`dropout`](forms.html#clock-emitting-forms) | the first reading arms quietly; 5s of silence publishes `stale true` once; the next reading publishes `stale false` once |
| settle | `debounce!` | publishes the latest value after 250ms of quiet (a trailing edge) |
| fixed cadence | `sample!` | publishes the latest value every second after the first match |
| dashboard numbers | `aggregate!` | publishes the 5-second average and rate on the clock, with or without new messages |
| heartbeat | [`every`](forms.html#heartbeats-with-every) | runs on a timer, with no message in scope |
| fleet roll-up | [funnel](forms.html#funnel) | a re-entrant rule republishes every sensor onto `clocked.all`, where `count!` and `aggregate!` keep one shared slot |

Run it and publish quickly to `clocked.sensor.temp`, then stop and watch the delayed subjects arrive:

```sh
examples/clocked.sh
```

Under `--soundcheck`, clock state lingers for 10 seconds after end of input so delayed forms can fire; `--soundcheck-linger-ms` changes that.
