description: Reference for every patchbay form: arguments, behaviour, state and worked snippets.

# Forms reference

{lede} Every patchbay form, grouped by what it is for. Each section has the signature, the behaviour on first sight and on nil, what state it keeps, and a snippet you can paste into a rule.

New to the language? Read the [patchbay guide](patchbay.html) first. Want whole files? See the [examples](examples.html).

## Form index

| family | forms |
|---|---|
| Top level | [`on`](#top-level-forms), [`every`](#heartbeats-with-every), [`lvc`](#top-level-forms), [`export`](#top-level-forms), [`import`](#top-level-forms) |
| Bound symbols | [`subject`, `payload`, `payload-float`, `payload-int`, `payload-bool`, `replaying?`, `reply-to`](#bound-symbols) |
| Control flow | [`if`, `when`, `and`, `or`, `do`](#special-forms), [`->`](#threading-with), [`not`](#comparisons-and-logic) |
| Comparison and arithmetic | [`=`, `<`, `<=`, `>`, `>=`](#comparisons-and-logic), [`+`, `-`, `*`, `/`, `min`, `max`, `abs`, `sign`, `clamp`](#arithmetic) |
| Numeric shaping | [`round`, `quantize`](#idempotent-filters) |
| Strings and subjects | [`subject-append`, `subject-with`, `subject-token`, `str-concat`, `contains?`, `starts-with?`, `ends-with?`, `now`](#strings-and-subjects) |
| Publishing | [`publish!`](#side-effects), [`reply!`](#request-reply), [`print!`](#debug-printing) |
| Value gates | [`squelch`, `deadband`, `changed?`, `delta`](#idempotent-filters) |
| Rate gates | [`hold-off`, `throttle`](#time-based-gates) |
| Edges and thresholds | [`rising-edge`, `falling-edge`, `transition`](#edge-gates), [`hysteresis`](#hysteresis) |
| Windowed aggregates | [`moving-avg`, `moving-sum`, `moving-min`, `moving-max`](#windowed-aggregates), [`ewma`](#exponential-average), [`rate`, `percentile`, `median`, `stddev`, `variance`](#rate-percentiles-distribution-shape) |
| Clock forms | [`on-silence`, `dropout`, `debounce!`, `sample!`, `aggregate!`](#clock-emitting-forms), [`every`](#heartbeats-with-every) |
| JSON | [`json-get`, `json-demux!`](#json-payloads) |
| Bars and counters | [`bar!`](#bars), [`count!`](#running-counters) |
| Cross-subject | [`last`](#last-values), [`:reentrant`](#re-entry), [the funnel pattern](#funnel) |

Conventions used throughout:

- A trailing `!` marks a form that emits or otherwise has an effect.
- `:ms N` is a wall-clock window; a bare `N` is a tick (sample) window.
- Stateful forms take the value **last**, so they thread with `->`.
- State is per call site and subject, and survives a [snapshot](state.html#snapshots) reload.

{{section docs/patchbay-cheatsheet.md "Top-level forms" level=2}}

Config forms are covered on their own pages: [`lvc`](state.html), [`export` and `import`](bridge.html).

{{section docs/patchbay.md "Values" level=2}}

{{section docs/patchbay.md "Bound symbols" level=2}}

{{section docs/patchbay.md "Special forms" level=2}}

`transition` and `dropout` are described under [Edge gates](#edge-gates) and [Clock-emitting forms](#clock-emitting-forms).

{{section docs/patchbay.md "Threading with `->`" level=2}}

{{section docs/patchbay.md "Comparisons and logic" level=2}}

{{section docs/patchbay.md "Arithmetic" level=2}}

{{section docs/patchbay.md "Strings and subjects" level=2}}

{{section docs/patchbay.md "Subject filters" level=2}}

{{section docs/patchbay.md "Side effects" level=2}}

{{section docs/patchbay.md "Re-entry" level=2}}

{{section docs/patchbay.md "Funnel" level=2}}

{{section docs/patchbay.md "Idempotent filters" level=2}}

{{section docs/patchbay.md "Time-based gates" level=2}}

{{section docs/patchbay.md "Edge gates" level=2}}

{{section docs/patchbay.md "Windowed aggregates" level=2}}

{{section docs/patchbay.md "JSON payloads" level=2}}

{{section docs/patchbay.md "Bars" level=2}}

{{section docs/patchbay.md "Running counters" level=2}}

{{section docs/patchbay.md "Last values" level=2}}

{{section docs/patchbay.md "Request-reply" level=2}}

{{section docs/patchbay.md "Debug printing" level=2}}

## Forms that are not there

Patchbay is deliberately small. There is no quote form, no macros, no user-defined functions and no lists-as-data: lists are calls, vectors are data. An unknown form or symbol is a load error, reported by `--validate` in every branch, so a typo cannot silently become text.
