description: Runnable patchbay examples graded from a five-line sensor rule to a multi-asset market data feed.

# Examples

{lede} Every example here is a real file in the repository, shown in full and checked with `monoblok --validate`. They are graded so you can start small and stop when you have what you need.

## How to run them

Each example in [`examples/`](https://github.com/lexvicacom/monoblok/tree/main/examples) has a matching `.sh` script that starts a temporary monoblok, subscribes, publishes sample data and prints what arrived. The scripts need the [NATS CLI](https://github.com/nats-io/natscli).

```sh
examples/sensors.sh
```

To run a patchbay yourself, pass the file to monoblok and use any NATS client:

```sh
monoblok examples/sensors.edn
nats sub 'sensors.>'
nats pub sensors.temp 42.04
```

To see what a file does with no server at all, pipe `SUBJECT|payload` rows through `--soundcheck`. The transcripts on these pages are produced this way when the site is built, so they are real output:

```sh
printf 'sensors.temp|42.01\nsensors.temp|42.04\n' | monoblok --soundcheck --soundcheck-label examples/sensors.edn
```

When testing with `nats pub`, pass negative bodies after `--`, for example `nats pub sensors.temp -- -5`.

## The examples by level

| example | level | what it shows |
|---|---|---|
| [`sensors.edn`](#round-and-squelch) | {tag:Starter} | round + squelch on a noisy sensor |
| [`lvc.edn`](#last-value-cache) | {tag:Starter} | `$LVC.>` replay: a late joiner gets the last value |
| [`demo.edn`](#a-tour-of-the-primitives) | {tag:Starter} | one rule for each primitive family |
| [`office-temp.edn`](examples-telemetry.html#office-temperature) | {tag:Intermediate} | moving-average alert and all-clear with `transition` and `hysteresis` |
| [`rental-car.edn`](examples-telemetry.html#rental-car-telemetry) | {tag:Intermediate} | quantize, deadband and an over-rev alert, in EDN and YAML |
| [`json-frames.edn`](examples-telemetry.html#json-frames-from-a-gateway) | {tag:Intermediate} | `json-demux!` a JSON device into scalar sub-subjects |
| [`ask.edn`](examples-telemetry.html#joins-and-request-reply) | {tag:Intermediate} | join two subjects with `last`, answer a request with `reply!` |
| [`clocked.edn`](examples-telemetry.html#clock-forms-and-a-funnel) | {tag:Advanced} | silence detection, debounce, sampling, clocked aggregates, `every`, a funnel |
| [`latency-stats.edn`](examples-telemetry.html#latency-statistics) | {tag:Intermediate} | live p50/p95/p99/stddev over a sliding window |
| [`ticker.edn`](examples-market.html#price-ticker) | {tag:Intermediate} | round, squelch, big-jump alerts, export |
| [`bars.edn`](examples-market.html#ohlc-bars) | {tag:Intermediate} | tick-count OHLC bars per symbol, alerting on closed bars |
| [`json-massive/`](examples-market.html#a-market-data-firehose) | {tag:Advanced} | a multi-asset JSON feed demuxed, cleaned and alerted on |
| [`bar-cardinality.edn`](examples-market.html#budgeting-bar-state) | {tag:Advanced} | measuring memory for bars across a large symbol universe |
| [`bridge.edn`](examples-nats.html#export-to-a-nats-server) | {tag:Intermediate} | export selected subjects to a real NATS server |
| [`import.edn`](examples-nats.html#tap-an-existing-nats-server) | {tag:Intermediate} | tap remote subjects, keep raw input private, export the clean stream |
| [`jetstream.yml`](examples-nats.html#jetstream-replay) | {tag:Advanced} | replay a populated JetStream stream, then continue live |
| [`moonwell_patchbay.edn`](examples-large.html#moonwell) | {tag:Advanced} | 24 rules over nested JSON telemetry, using most of the language |
| [`cloudhook_patchbay.edn`](examples-large.html#cloudhook) | {tag:Advanced} | 23 rules: a second large generated patchbay |

## Round and squelch

{tag:Starter} The canonical first rule. A jittery sensor publishes readings that differ in the second decimal place; subscribers only care when the value really moves.

{{include examples/sensors.edn}}

`payload-float` parses the payload as a number, [`round`](forms.html#idempotent-filters) trims it to one decimal place, [`squelch`](forms.html#idempotent-filters) passes it only if it differs from the last value seen on this subject, and [`publish!`](forms.html#side-effects) sends it to `<subject>.stable`. When `squelch` suppresses, it returns nil, and `publish!` on nil does nothing.

```soundcheck examples/sensors.edn
sensors.temp|42.01
sensors.temp|42.04
sensors.temp|42.08
sensors.temp|42.12
sensors.temp|43.00
sensors.temp|43.00
sensors.temp|43.01
```

Seven readings in, three out. State is per subject, so `sensors.temp` and `sensors.humidity` are deduplicated independently by the same rule.

## Last-value cache

{tag:Starter} A normal subscriber sees nothing until the next publish. The last-value cache fixes that: opt subjects in with `(lvc ...)`, and a subscriber to `$LVC.<subject>` gets the cached value immediately, then live updates.

{{include examples/lvc.edn}}

```sh
# Publish a value, then walk away.
nats pub sensors.temp 23.5

# Later, subscribe. A normal SUB would see nothing until the next publish.
nats sub '$LVC.sensors.temp'
# -> prints "23.5" immediately
```

Single-quote any subject starting with `$` so your shell does not try to expand it. The cache pairs particularly well with conditioning rules: a subscriber to `$LVC.sensors.temp.stable` gets the most recent rule-produced value on connect, not the raw input. More in [LVC, stats and snapshots](state.html).

## A tour of the primitives

{tag:Starter} This is the patchbay loaded on the public demo server. It gives one compact example of every primitive family so you hit all of them in a single sitting: `squelch` for value dedup, `deadband` for "ignore small wobbles", `moving-avg` for smoothing, `delta` for per-tick change, `transition` for boolean edge detection, `rising-edge` for one-shot crossings, `hold-off` for time-based rate limiting, and `count!` for tallies.

{{include examples/demo.edn}}

Try it against the public server without installing anything:

```sh
nats context save monoblok-demo --server nats://demo.monoblok.host:4222
nats context select monoblok-demo

# One terminal
nats sub 'demo.>'

# Another
nats pub demo.sensors.temp 25.3
nats pub demo.sensors.temp 25.34
nats pub demo.sensors.temp 25.38
```

The first terminal prints a handful of derived subjects: `demo.sensors.temp.stable`, `.delta`, `.smoothed`, `.delta-abs`. Each one comes from a different patchbay rule applied to the input. The client publishing the raw readings does not know any of those rules exist.

| publish here | watch here | what the rule does |
|---|---|---|
| `demo.sensors.<name>` (a number) | `demo.sensors.<name>.stable` | `round 1` then `squelch` (emit when the rounded value moves) |
| | `demo.sensors.<name>.delta` | `deadband 0.5` (suppress moves smaller than 0.5) |
| | `demo.sensors.<name>.smoothed` | `moving-avg 10` then `deadband 1.0` |
| | `demo.sensors.<name>.avg5s` | `moving-avg :ms 5000`, a 5-second sliding mean |
| | `demo.sensors.<name>.delta-abs` | per-tick numeric `delta` (0 on first sight) |
| | `demo.sensors.<name>.alert` / `.ok` | `transition` across 28.0: "hot" rising, "cool" falling |
| | `demo.sensors.<name>.overload` | `hold-off 2000` while above 40 (one emit per 2s) |
| | `demo.sensors.<name>.spike` | `rising-edge` across 50.0 |
| | `demo.sensors.<name>.count` | `count!` of false-to-true crossings above 50.0 |
| `demo.log.<name>` (any text) | `demo.alerts` | mirror if the payload contains `alert`, prefixed with the source |

The same file exists as [`demo.yml`](https://github.com/lexvicacom/monoblok/blob/main/examples/demo.yml) and [`demo.json`](https://github.com/lexvicacom/monoblok/blob/main/examples/demo.json) if you want to compare the three spellings rule by rule. The demo server is shared and public with no auth, so do not send secrets. The [demo walkthrough](https://github.com/lexvicacom/monoblok/blob/main/docs/demo.md) has more to try, and the [live demo](../demo.html) page drives the same server from your browser.
