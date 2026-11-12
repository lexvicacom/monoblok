description: Two large generated patchbays, Moonwell and Cloudhook, that use most of the language over nested JSON telemetry.

# Large patchbays

{lede} Two patchbays of two dozen rules each, showing how the small forms compose into a whole conditioning layer: JSON demux at the head, stable dashboard channels, safety alerts, roll-ups, liveness and audit trails.

Both files live in [`advanced-examples/`](https://github.com/lexvicacom/monoblok/tree/main/advanced-examples). They were written by a coding agent with [`AGENTS_PATCHBAY.md`](https://github.com/lexvicacom/monoblok/blob/main/docs/AGENTS_PATCHBAY.md) loaded, for deliberately fictional domains. They demonstrate rule composition and can be validated and run locally; their fictional domain models are not intended for production use. Each has a matching Python pump script that publishes randomised telemetry.

## How a large patchbay is laid out

Both files follow the same order, and it is a good template for your own:

1. **Cache.** One `(lvc ...)` form opts the input, alert, ops and audit trees into the last-value cache, so a dashboard that connects late is populated at once.
2. **Demux.** One `:reentrant true` rule per JSON frame type breaks the frame into scalar subjects. Everything after this works on plain numbers and strings.
3. **Stable channels.** Short `->` pipelines (smooth, round, gate, publish) give dashboards quiet subjects to subscribe to.
4. **Liveness.** `sample!` republishes the latest frame on a fixed cadence and `dropout` flags a rig that has gone silent.
5. **Alerts.** `transition` over a windowed statistic raises one alert and one all-clear; `rising-edge` plus `throttle` caps one-shot alerts.
6. **Roll-ups.** `aggregate!`, `bar!` and `count!` produce the numbers a report wants.
7. **Audit.** Rules build dated subjects with `subject-with` and `now`, so events land on subjects like `audit.2026-11-11.rig-03.seal`.
8. **Fleet meter.** One rule on the whole tree publishes the ingest rate.

## Moonwell

{tag:Advanced} A fictional lunar ice-mining habitat. Rigs publish three nested JSON frames (`env`, `drill`, `power`) and free-form log lines under `moonwell.<rig>.*`.

{{include advanced-examples/moonwell_patchbay.edn}}

Forms worth studying here:

| rule | technique |
|---|---|
| `moonwell.*.env` demux | [`json-demux!`](forms.html#json-payloads) with `[PATH SUFFIX]` pairs flattens nested paths such as `sensor.thermal.temp` to a short suffix (`temp`) |
| `env.temp` stable | a time window feeding value gates: `moving-avg :ms 45000`, then `round`, then `squelch` |
| `env` liveness | two effects in one `do`: [`sample!`](forms.html#clock-emitting-forms) for a 5-second snapshot and `dropout` for stale and recovered |
| `env.dust` alert | a boolean threaded through [`rising-edge`](forms.html#edge-gates) then [`throttle :ms 300000 2`](forms.html#time-based-gates): at most two dust alerts in five minutes |
| `env.radiation` alert | [`transition`](forms.html#edge-gates) over a 95th [`percentile`](forms.html#rate-percentiles-distribution-shape), so one outlier does not raise the alarm |
| `env.seal` audit | [`changed?`](forms.html#idempotent-filters) as a predicate, publishing to a dated subject built with `subject-with` and `(now :date)` |
| `drill.torque` chatter | spread over a window: `moving-max` minus `moving-min` |
| `drill.mode` | vector membership with [`contains?`](forms.html#strings-and-subjects), a [`count!`](forms.html#running-counters) and an alert in one `do` |
| `env.ice` | three clock forms on one subject: `debounce!`, `aggregate!` and a one-minute `bar!` |
| `moonwell.>` meter | [`rate :ms 1000`](forms.html#rate-percentiles-distribution-shape) across the whole tree for a fleet-wide messages-per-second figure |

```sh
cd advanced-examples
monoblok moonwell_patchbay.edn
python pump_moonwell.py
```

A [recorded session](https://lexvicacom.github.io/monoblok/show-n-tell/moonwell_linkedin_demo.html) shows a coding assistant building this patchbay.

## Cloudhook

{tag:Advanced} A fictional offshore fleet of autonomous kite turbines. Kites publish flat JSON frames for flight, weather and power under `cloudhook.<kite>.*`.

{{include advanced-examples/cloudhook_patchbay.edn}}

The structure matches Moonwell, with a few different techniques:

- **Compound conditions in a `transition`.** The altitude and voltage rules use `(or (< avg LO) (> avg HI))` to alert when a smoothed value leaves a band in either direction. Each `moving-avg` inside the `or` is its own call site with its own ring.
- **Operator commands as subjects.** The tether and shear rules publish to `ops.<kite>.winch.ease` and `ops.<kite>.pattern.widen`: the alert is addressed to whatever acts on it.
- **Integer payloads.** `(= payload-int 1)` through `rising-edge` records the moment a brake engages, once.
- **Quantised bands.** `moving-avg`, `quantize 10`, `squelch` reports altitude in 10-unit bands, so a dashboard updates only when the kite changes band.

```sh
cd advanced-examples
monoblok cloudhook_patchbay.edn
python pump_cloudhook.py
```

The pump scripts default to `127.0.0.1:4222`. Use `--host` and `--port` if monoblok is listening somewhere else.

## Making your own

To make more examples like these, give your coding agent the patchbay-specific instructions first. Append `docs/AGENTS_PATCHBAY.md` to an `AGENTS.md` (or `CLAUDE.md`) in the directory where you want the agent to work:

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/docs/AGENTS_PATCHBAY.md >> ./AGENTS.md
```

[Writing patchbays with an agent](agents.html) covers the workflow and what to check in the result. Then ask for a complete pair: one patchbay `.edn` file and one pump script that publishes matching test data.

```text
Create a patchbay .edn file and a matching Python script that publishes test data.

Use a fictional telemetry domain, optionally with JSON input subjects, scalar
demux rules, alerts, stable dashboard outputs, audit subjects, and a few
windowed stats. The pump script should connect to monoblok on 127.0.0.1:4222
and publish randomised test data for the subjects the patchbay expects.
```

Check whatever comes back before running it:

```sh
monoblok --validate your_example_patchbay.edn
```

## Sizing a large patchbay

- Cost follows the rules that **match** a message, not the number of rules in the file. Narrow filters keep the per-message work small.
- Each re-entrant demux adds one round of rule evaluation per emitted field. Demux only the fields that later rules or subscribers use.
- Every stateful form keeps a slot per concrete subject. A wildcard over thousands of devices is thousands of slots; see [Budgeting bar state](examples-market.html#budgeting-bar-state) for how to measure it.
- `(now :minute)` in a subject creates a new subject every minute. Prefer `:hour` or `:date` unless you mean it.
