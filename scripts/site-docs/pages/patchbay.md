description: The patchbay rule language: rule shape, threading, state, re-entry, file formats and debugging.

# Patchbay guide

{lede} patchbay is a small S-expression DSL describing how every incoming publish gets filtered, conditioned, and re-routed. It is shared by the monoblok server and tinyblok on microcontrollers.

This page explains how the language fits together. For what each operator does, see the [forms reference](forms.html); for complete files, see the [examples](examples.html).

## A first rule

Rules are top-level `(on SUBJECT-FILTER BODY)` forms. The body is evaluated whenever an incoming subject matches the filter. Wildcards are NATS-style: `*` matches one token, `>` matches the tail.

```edn
(on "sensors.*"
  (when (> payload-float 30.0)
    (publish! (subject-append "high") payload)))

; Round to 1dp, drop duplicates, emit on the .stable sub-subject.
(on "sensors.*"
  (-> payload-float
      (round 1)
      (squelch)
      (publish! (subject-append "stable"))))
```

The first rule copies any reading above 30 to `sensors.<name>.high`. The second rounds to one decimal place, drops the value if it has not changed, and republishes to `sensors.<name>.stable`. Both rules see every `sensors.*` message; rules do not consume messages or stop each other.

The vocabulary is borrowed from electronics (`squelch` suppresses until the value changes, `deadband` ignores movement smaller than a threshold) because the names already mean the right thing. A "patchbay" in a studio is a grid of jacks you wire between sources and destinations, which is exactly what the DSL looks like on the page.

{{section docs/patchbay.md "S-expression syntax in 30 seconds" level=2}}

## The shape of a patchbay file

A file is a list of top-level forms. Rules sit beside a handful of config forms:

| form | role |
|---|---|
| `(on FILTER BODY)` | a rule: run BODY for each message whose subject matches FILTER |
| `(every :ms N BODY...)` | a timer rule: run BODY every N milliseconds |
| `(lvc [FILTER ...])` | opt subjects into the [last-value cache](state.html) |
| `(export ...)` | forward matching publishes to a real NATS cluster ([export](bridge.html#export)) |
| `(import ...)` | feed remote NATS or JetStream messages into the rules ([import](bridge.html#import)) |

Inside a rule body a handful of **bound symbols** stand for the current message: `subject`, `payload`, `payload-float`, `payload-int`, `payload-bool`, `reply-to` and `replaying?`. The [reference](forms.html#bound-symbols) lists each one.

Forms fall into a few families, and most rules are a short chain drawn from them:

| family | examples | what they do |
|---|---|---|
| pure transforms | `round`, `quantize`, `clamp`, `abs`, arithmetic | reshape a number |
| gates | `squelch`, `deadband`, `hold-off`, `throttle` | pass a value or suppress it (return nil) |
| edges | `rising-edge`, `falling-edge`, `transition`, `hysteresis` | fire once when a condition flips |
| windows | `moving-avg`, `percentile`, `stddev`, `rate`, `ewma` | summarise the last N samples or the last N ms |
| clock forms | `on-silence`, `dropout`, `debounce!`, `sample!`, `aggregate!`, `every` | act when time passes, not only when a message arrives |
| JSON | `json-get`, `json-demux!` | read fields out of JSON payloads |
| effects | `publish!`, `reply!`, `count!`, `bar!`, `print!` | put bytes on the wire (the `!` forms) |

Scan a rule and the bangs are the lines that publish; everything else is pure or value-returning.

## Pipelines

Most rules read top to bottom as a pipeline, written with `->`. Each step receives the previous value as its **last** argument:

```edn
(-> payload-float
    (round 1)
    (squelch)
    (publish! (subject-append "stable")))
```

Gates return their input on pass and `nil` on suppress, and `publish!` is a no-op on nil, so a chain of gates stops by itself. You do not need `when` anywhere in a threaded pipeline. [Threading with ->](forms.html#threading-with) has the full expansion rules.

## State

Stateful forms (gates, windows, counters, bars) remember things between messages. The rules for that memory are short:

- **State is per call site and subject.** Each place a stateful form appears in a rule body gets its own state, so two `(squelch ...)` calls in one rule never share a slot. A rule on `sensors.*` keeps one slot for every concrete subject it has seen.
- **The first message on a new subject passes gates.** There is no prior value to compare against, so `squelch` and `deadband` emit; edge forms such as `rising-edge` stay quiet because no edge has happened yet.
- **There is no shared state across subjects.** To combine subjects, publish them onto one subject and keep the state there. That is the [funnel](forms.html#funnel) pattern.
- **State survives restarts** when [snapshots](state.html#snapshots) are enabled.

Because every distinct subject gets a slot, avoid unbounded subject tokens such as timestamps, user IDs, or raw device-generated strings unless you intend to create state for each one.

## Rules feeding rules

By default, a publish from inside a rule is delivered to subscribers and the LVC, but is not matched against other rules. One inbound message produces one round of rule evaluation.

Mark a rule `:reentrant true` and its emissions are matched against the other rules too. That is how staged pipelines are built: one rule demuxes a JSON frame into scalar subjects, and later rules condition each scalar.

```edn
(on "devices.*" :reentrant true
  (json-demux! "temp" "hum" payload))

(on "devices.*.temp"
  (-> payload-float
      (round 0)
      (publish! (subject-append "stable"))))
```

A rule that emits a subject which matches itself is a feedback loop: howl, except in subject-space. Re-entry is capped at depth 8, long enough for any reasonable staged pipeline (demux, condition, mirror, alert is 3 or 4) and short enough that a misconfigured rule terminates, with a warning, almost immediately. See [Re-entry](forms.html#re-entry).

## File formats

EDN is canonical for hand-written patchbays. `.json` files are accepted for tooling compatibility, and `.yml` / `.yaml` files use a small YAML sugar layer. All three load into the same AST, so the choice is about who edits the file.

The same rule three ways:

```edn
(on "car.*.rpm"
  (-> payload-float
      (quantize 50)
      (squelch)
      (publish! (subject-append "stable") :dp 1)))
```

```yaml
on:
  - sub: "car.*.rpm"
    thread:
      - $payload-float
      - [quantize, 50]
      - [squelch]
      - [publish!, [subject-append, "stable"], dp, 1]
```

```json
[
  ["on", "car.*.rpm",
    ["->", "$payload-float", ["quantize", 50], ["squelch"],
      ["publish!", ["subject-append", "stable"], {"dp": 1}]]]
]
```

### Side by side

{{between docs/patchbay-cheatsheet.md "| EDN | JSON | YAML |" "with `servers:` and `subject:`"}}

{{section docs/patchbay.md "YAML sugar files" level=3}}

{{section docs/patchbay.md "JSON patchbay files" level=3}}

{{section docs/patchbay.md "EDN is the notation; the patchbay is the evaluator" level=3}}

{{section docs/overview.md "Validate and debug rules" level=2}}

{{section docs/overview.md "`--trace`: protocol trace" level=3}}

### Stage by stage with print!

`(print! LABEL X)` writes one line to stderr and returns `X` unchanged, so it can sit between any two steps of a pipeline. See [Debug printing](forms.html#debug-printing).

### Watching rule counters

The server publishes `emitted` and `suppressed` totals for every rule on [`$STATS.rules.<i>.*`](state.html#stats-live-counters), so you can confirm a gate is actually firing (or not).

```sh
nats sub '$STATS.>'
```

{{section docs/overview.md "Coding assistants" level=2}}

Two generated patchbays of this kind are walked through in [Large patchbays](examples-large.html).
