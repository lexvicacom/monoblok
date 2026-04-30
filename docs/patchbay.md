# patchbay reference

The [README](./README.md) covers what patchbay is and what it's for.
This doc is the full reference for the DSL: syntax, bound symbols, and
every operator. For the 30-second pitch and worked examples, start
there; for "what does `(deadband ...)` actually do," this is the page.
For one-line summaries of every form, see the
[cheatsheet](./patchbay-cheatsheet.md). Runnable patchbay files for
common scenarios live in [`examples/`](./examples/).

## S-expression syntax in 30 seconds

If the parens look alien: every list `(head arg1 arg2 ...)` is a call
where `head` is the operator and the rest are its arguments. No commas,
no infix, no precedence to memorise; `(+ 1 2 3)` is `1 + 2 + 3` and
`(> x 10)` is `x > 10`. Nesting is just a list inside a list:
`(publish (subject-append "high") payload)` calls `publish` with two
args, the first of which is itself the result of calling
`subject-append`. Strings are double-quoted, numbers bare, and
everything else (`subject`, `payload`, `when`, `+`) is a symbol
resolved by the evaluator.

That's the whole grammar. The rest of this doc is just which
operators exist and what they do.

### Lists are calls only inside rule bodies

If you've used Clojure, the muscle memory is "every `(...)` is a call
unless I `'`-quote it." Patchbay is not quite that. There is no quote
form. Whether a `(...)` list is "evaluated as a call" depends purely
on where it appears:

- Inside an `(on FILTER BODY)` body, every list dispatches on its head
  symbol (`hold-off`, `publish`, `+`, etc.). Unknown heads error.
- Inside the top-level `(bridge ...)` form, after a keyword like
  `:servers` or `:export`, a list is read as a literal sequence of
  values. `(:servers ("nats://a:4222" "nats://b:4222") ...)` is a
  two-element list of strings, not a call to `nats://a:4222`. Same
  underlying parser, different consumer.

In other words: the sexpr layer just gives you nested lists. The rule
evaluator interprets lists as calls; the bridge config reader
interprets them as vectors. You never need to quote anything.

## Values

`nil`, booleans (`true` / `false`), numbers (parsed as `f64`),
strings (`"..."`), symbols, and lists. Truthiness: `nil` and `false`
are falsy, everything else (including `0` and `""`) is truthy.

## Bound symbols

Bare symbols inside a body that resolve against the current message:

| symbol          | type     | value                              |
|-----------------|----------|------------------------------------|
| `subject`       | string   | the incoming subject               |
| `payload`       | string   | the raw payload bytes              |
| `payload-float` | number   | `payload` parsed as a float (errors if not numeric) |
| `payload-int`   | number   | `payload` parsed as a signed integer (errors on non-integer input) |

## Special forms

Evaluate their arguments lazily / with short-circuiting.

| form                        | behavior                                                     |
|-----------------------------|--------------------------------------------------------------|
| `(if C T E?)`               | `T` if `C` is truthy, else `E` (or `nil` if omitted)         |
| `(when C BODY...)`          | evaluate `BODY` sequentially iff `C` is truthy               |
| `(and X...)`                | left-to-right, returns the first falsy value or the last     |
| `(or  X...)`                | left-to-right, returns the first truthy value or the last    |
| `(do  X...)`                | evaluate in order, return the last                           |
| `(-> X F...)`               | thread `X` as the **last** arg of each `F` (see below)       |
| `(transition C UP DOWN)`    | eval `UP` on `C` false→true, `DOWN` on true→false, else nil  |

## Comparisons and logic

All comparisons are chained: `(< a b c)` means `a < b && b < c`.
Numeric comparisons coerce string args via `parseFloat`; `=` is
tag-strict (a `number` never equals a `string`).

| form              | notes                                                    |
|-------------------|----------------------------------------------------------|
| `(= a b ...)`     | all equal; same-tag, deep-equal for strings/symbols      |
| `(> a b ...)`     | chained numeric                                          |
| `(< a b ...)`     | chained numeric                                          |
| `(>= a b ...)`    | chained numeric                                          |
| `(<= a b ...)`    | chained numeric                                          |
| `(not x)`         | boolean negation on truthiness                           |

## Arithmetic

Variadic, left-fold, numeric. Single-arg variants follow Clojure:
`(- x)` negates, `(/ x)` reciprocates, `(+ x)` / `(* x)` are identity.

`(+ a b ...)` `(- a b ...)` `(* a b ...)` `(/ a b ...)`

Also: `(min a b ...)`, `(max a b ...)` (variadic, at least 1 arg),
`(abs x)`, `(sign x)` (returns -1, 0, 1), `(clamp LO HI X)` (value last
so it threads).

## Strings and subjects

| form                             | result                                              |
|----------------------------------|-----------------------------------------------------|
| `(str-concat a b ...)`           | concatenates string/symbol args                     |
| `(subject-append "suffix")`      | `"<current-subject>.suffix"`                        |
| `(contains? haystack needle)`    | boolean substring check                             |
| `(starts-with? haystack needle)` | boolean prefix check                                |
| `(ends-with? haystack needle)`   | boolean suffix check                                |
| `(subject-token N [S])`          | Nth dot-separated token (0-indexed) of `S` (default: current subject); nil if out of range |

## Subject filters

Filters on `(on FILTER ...)` forms and in `SUB` subjects follow the
NATS convention:

| token | meaning                                                       |
|-------|---------------------------------------------------------------|
| `*`   | exactly one token in this position (any characters)           |
| `>`   | one or more tokens, matches the rest of the subject           |

`*` can appear in any position; `>` must be the **last** token and
consumes everything after it. `foo.>` matches `foo.a` and
`foo.a.b.c.d`. `foo.*` matches `foo.a` only — a filter with N tokens
and no `>` requires exactly N tokens in the subject. If you need
"anything starting with `bip` with at least three tokens after it,"
that's `bip.*.*.*` or `bip.*.*.*.>`, not `bip.>` (which matches
`bip.a` too). Putting `>` anywhere except the tail is a validation
error, not a silent reinterpretation.

## Side effects

`(publish SUBJECT PAYLOAD)` validates `SUBJECT` as a publishable
subject (no wildcards, no `$LVC.*`) and enqueues a fan-out. Returns
`nil`. Publishes from rules participate in normal delivery + LVC
caching but are not themselves fed back through rule evaluation.

`(publish-to SUBJECT VALUE)` is the same thing with args flipped so it
slots on the tail of a `->` pipeline. Coerces numeric `VALUE` to its
canonical string form. If `VALUE` is `nil` (which is what a gate
returns when it suppresses), `publish-to` is a no-op. That nil
short-circuit is what makes the pipeline form below read
top-to-bottom.

## Threading with `->`

`(-> X f1 f2 ...)` threads `X` as the **last** argument of each `fN`.
A bare symbol `f` is treated as the call `(f)`. Last-arg threading
fits this dialect because every stateful/transform op takes the value
last:

```edn
(-> payload-float
    (round 1)
    (squelch)
    (publish-to (subject-append "stable")))
```

expands to:

```edn
(publish-to (subject-append "stable") (squelch (round 1 payload-float)))
```

The gates (`squelch`, `deadband`) return the value on pass and `nil`
on suppress, so `publish-to` at the tail becomes a no-op when the
gate blocks. You don't need `when` anywhere in a threaded pipeline.

## Idempotent filters

Real sensor streams are noisy. These primitives turn a chatty
publisher into a "change-only" one without any timers or windows;
one slot of state per subject the rule has ever seen.

| form                  | behavior                                                                       |
|-----------------------|--------------------------------------------------------------------------------|
| `(round N X)`         | round number `X` to `N` decimal places (pure)                                  |
| `(quantize STEP X)`   | snap `X` to the nearest multiple of `STEP` (pure)                              |
| `(squelch X)`         | pass `X` through iff it differs from the last `X` seen on this (rule, subject) |
| `(deadband DELTA X)`  | pass `X` through iff numeric `X` changed by at least `DELTA` since last emit   |
| `(changed? X)`        | boolean predicate: true iff `X` differs from the last `X` on this (rule, subject); true on first sight |
| `(delta X)`           | numeric difference between `X` and the last `X`; 0 on first sight              |

`changed?` is the predicate form of `squelch`: it returns a plain
boolean instead of value-or-nil, which composes cleanly inside
`if`, `and`, `or` without relying on the nil short-circuit.

Gates return the value on pass and `nil` on suppress. Truthy-on-pass
keeps them usable as conditions in `when` / `and`, while passing the
value through makes them compose directly with `->` and `publish-to`.
`squelch` stores the stringified value; `deadband` stores the numeric
anchor and only updates it on an accepted change. Both are **per rule,
per subject**: two rules watching the same subject don't interfere,
and the first message a rule sees on a new subject always passes.

```edn
; Jittery temperature sensor, only emit when the rounded value moves.
(on "sensors.*"
  (-> payload-float
      (round 1)
      (squelch)
      (publish-to (subject-append "stable"))))

; Analog deadband: suppress changes below 0.5.
(on "sensors.*"
  (-> payload-float
      (deadband 0.5)
      (publish-to (subject-append "delta"))))
```

## Time-based gates

Where `squelch` and `deadband` dedupe by *value*, `hold-off` dedupes by
*time*: fire once, then ignore further triggers for a fixed interval.
Radar term for the same idea (the re-arm timer after a pulse). Useful
when the upstream is chatty at a faster cadence than downstream
consumers want, and you don't care which of the suppressed samples you
drop.

| form                 | behavior                                                                     |
|----------------------|------------------------------------------------------------------------------|
| `(hold-off MS X)`    | pass `X` through on first sight, then suppress further `X` for `MS` ms       |

Time source is the ingress wall-clock (monotonic ms from the event
loop), stamped once per incoming publish, so every op in one evaluation
sees the same "now." This means `hold-off` is only evaluated when a
message arrives (it's not a ticking timer): a stream that goes silent
will stay silent, not fire a delayed trailing emit. Per (rule, subject),
same as the other gates.

```edn
; Alert is noisy during a transient; emit at most once per 2s.
(on "sensors.*"
  (when (> payload-float 80.0)
    (-> payload
        (hold-off 2000)
        (publish-to (subject-append "alert")))))
```

Stacks cleanly with value-based gates when you want both: "only if the
value changed, and at most once per 500ms."

```edn
(-> payload-float
    (round 1)
    (squelch)
    (hold-off 500)
    (publish-to (subject-append "stable")))
```

`(throttle WINDOW MAX X)` is the related but distinct gate for "at most
MAX events per WINDOW." `hold-off` enforces a minimum interval between
passes (one timer that resets each pass); `throttle` enforces a maximum
count over a sliding window (a bucket of recent pass timestamps).
Reach for `hold-off` when you want to debounce a chattery edge, and
`throttle` when you want to cap a steady stream's rate without caring
about the spacing.

```edn
; Up to 5 alerts per minute, regardless of how they arrive.
(on "alerts.>"
  (-> payload
      (throttle (window-ms 60000) 5)
      (publish-to (subject-append "rate-limited"))))
```

`WINDOW` is `(ticks N)` or `(window-ms N)`. Tick form counts the last
`N` evaluations; time form counts pass timestamps in the last `N` ms
(walker-evicted). Per (rule, subject, window-kind).

## Windowed aggregates

A small windowing family for smoothing and window-based triggers. Every
call site gets its own buffer, keyed by `(rule, subject, op, window
kind)`, so a tick-windowed and a time-windowed `moving-avg` on the same
rule and subject keep independent state.

A window is either a tick count or a wall-clock duration. Window
descriptors are values, returned by two pure builtins:

| form              | returns                | meaning                              |
|-------------------|------------------------|--------------------------------------|
| `(ticks N)`       | window descriptor      | last N samples (fixed-cap ring)      |
| `(window-ms N)`   | window descriptor      | last N ms of wall-clock time         |

Wall-clock time is the ingress timestamp stamped once per inbound PUB
(same source `hold-off` reads). For windows that elapse without a new
PUB, the server's clock walker (~500ms cadence) evicts old samples and
closes any time-windowed `bar` whose window has fully passed, so a
quiet feed doesn't leave you with a stale aggregate or an unflushed
bar.

| form                       | returns | cost                                |
|----------------------------|---------|-------------------------------------|
| `(moving-avg WINDOW X)`    | number  | O(1) per update for `(ticks N)`     |
|                            |         | O(n) per update for `(window-ms N)` |
| `(moving-sum WINDOW X)`    | number  | same as `moving-avg`                |
| `(moving-max WINDOW X)`    | number  | O(1) amortized for `(ticks N)`      |
|                            |         | O(n) per update for `(window-ms N)` |
| `(moving-min WINDOW X)`    | number  | same as `moving-max`                |

Tick rings allocate `N × 8 B` plus a small deque for max/min and `N` is
fixed at first call. Time rings grow with event rate × window: storage
is unbounded in principle, bounded in practice by however many samples
arrive inside the window. Aggregate readers do an O(n) scan over the
live window, which is fine at sensor / ticker rates; if you want
sub-microsecond updates over wide time windows, prefer `(ticks N)` with
a generous `N`.

```edn
; Smooth with a 10-sample moving average, and only emit when the
; smoothed value drifts by at least 1.0.
(on "sensors.*"
  (-> payload-float
      (moving-avg (ticks 10))
      (deadband 1.0)
      (publish-to (subject-append "smoothed"))))

; Same idea, but window over the last 5 seconds of wall-clock time
; instead of the last N samples. Useful when sample cadence varies and
; you want a stable "last 5 seconds" view.
(on "sensors.*"
  (-> payload-float
      (moving-avg (window-ms 5000))
      (publish-to (subject-append "avg5s"))))

; Volatility detector: alert if the spread over the last 20 samples
; exceeds 5 units. Ring ops return numbers, so the non-threaded form
; is often clearer when you need multiple windows at once.
(on "sensors.*"
  (when (> (- (moving-max (ticks 20) payload-float)
              (moving-min (ticks 20) payload-float)) 5.0)
    (publish (subject-append "volatile") payload)))
```

Window ops return numbers, so they compose directly with the
arithmetic, comparison, and gating primitives above. No special
pipeline syntax.

### Rate, percentiles, distribution shape

The same windowing machinery powers a few more analytical primitives.
Each one is keyed `(rule, op, window-kind, subject)` so they don't
share state with `moving-*` even on the same subject.

| form                          | returns | meaning                                       |
|-------------------------------|---------|-----------------------------------------------|
| `(rate WINDOW X)`             | number  | events per second over WINDOW. Time-only.     |
| `(percentile WINDOW P X)`     | number  | Pth percentile of X (P in [0, 1])             |
| `(median WINDOW X)`           | number  | sugar for `(percentile WINDOW 0.5 X)`         |
| `(stddev WINDOW X)`           | number  | population stddev of X over WINDOW            |
| `(variance WINDOW X)`         | number  | population variance of X over WINDOW          |

`rate` requires `(window-ms N)` — "rate over the last N samples" has
no time unit, so passing `(ticks N)` is a type error. `X` is evaluated
but its value ignored: the op counts pushes, not magnitudes. Useful
for "events/sec on this subject," "alerts/sec," etc.

`percentile`, `median`, `stddev`, and `variance` all read the window
and return immediately — no warm-up. On the first sample they return
that sample (or 0 for variance/stddev). Cost is O(n log n) for
percentile/median (sort copy) and O(n) for stddev/variance.

```edn
; Latency p99 over the last 200 samples. Useful in front of a SLO
; gate: only alert when sustained tail behaviour is bad, not a single
; outlier.
(on "rpc.latency.*"
  (-> payload-float
      (percentile (ticks 200) 0.99)
      (publish-to (subject-append "p99"))))

; Live event rate per subject, expressed in Hz.
(on "events.>"
  (-> payload
      (rate (window-ms 1000))
      (publish-to (subject-append "rate-hz"))))

; Volatility flag: alert if the price stddev over the last minute
; jumps above 0.5.
(on "MARKET.*"
  (when (> (stddev (window-ms 60000) payload-float) 0.5)
    (publish (subject-append "volatile") payload)))
```

## Edge gates

`squelch` fires on every change; often you only want one fire per
transition in a specific direction (alert on cross-up, all-clear on
cross-down). Edge gates take a boolean and pass it through only on the
matching transition.

| form                  | fires when                                     |
|-----------------------|------------------------------------------------|
| `(rising-edge X)`     | `X` transitions from falsy to truthy           |
| `(falling-edge X)`    | `X` transitions from truthy to falsy           |

Both are **per rule, per subject**, and the first message a rule sees
on a new subject never fires (no prior state means no edge, so no
cold-start noise). Use `rising-edge` / `falling-edge` when you only
care about one direction and want to keep threading through `->`.

When you want both directions (alert + all-clear), `transition` is the
condensed form: one rule, one boolean, one shared `prev` slot, two
branches.

```edn
; Alert on cross-up, all-clear on cross-down, in a single rule.
(on "temp.*.*"
  (transition (> (moving-avg (ticks 60) payload-float) 28.0)
    (publish-to (subject-append "alert") "hot")
    (publish-to (subject-append "ok")    "cool")))
```

Compared to pairing two edge-gate rules, this evaluates the predicate
once, keeps one `moving-avg` ring instead of two, and doesn't renumber
when you later add or remove rules around it.

## JSON payloads

Most off-the-shelf sensors and gateways emit JSON frames (`{"temp":12.04,"hum":80}`),
not bare scalars. Two ops bridge that gap. Both look up keys against
the **top-level** object only (no JSON path, no nesting, no array
indexing) and use Zig's `std.json.Scanner` so escapes and `\uXXXX` are
handled correctly.

| form                          | what it does                                                                |
|-------------------------------|-----------------------------------------------------------------------------|
| `(json-get KEY PAYLOAD)`      | return that field's value as a number, string, or boolean. Nil otherwise.   |
| `(json-demux KEY ... PAYLOAD)` | publish each KEY's value to `<subject>.<key>`. Side-effecting. Returns nil. |

`json-get` returns `nil` whenever the field can't be turned into a
scalar (key missing, payload not a JSON object, value is `null`, value
is a nested object/array). That nil flows through `publish-to` as a
no-op, so a JSON-aware pipeline reads exactly like the analog ones:

```edn
; Treat a JSON sensor frame like a scalar stream.
(on "sensors.*"
  (-> payload
      (json-get "temp")
      (round 1)
      (squelch)
      (publish-to (subject-append "temp.stable"))))
```

`json-demux` is the breakout: one wire carrying a multi-field frame
fanned out to one sub-subject per field, like a demultiplexer chip
selecting an output line per key. Note that the publishing is
implicit, the form has no return value to thread on. Useful as the
first rule for JSON-publishing devices, so the rest of the patchbay
can stay scalar.

```edn
; Break a multi-field frame out into per-field subjects.
; sensors.foo {"temp":12.5,"hum":80} -> sensors.foo.temp 12.5
;                                       sensors.foo.hum  80
(on "sensors.*"
  (json-demux "temp" "hum" payload))
```

If a key is missing, null, or holds a nested value, `json-demux` skips
it silently rather than emitting an error. Numbers come out canonically
formatted, strings are unquoted, booleans render as `true` / `false`.

Both ops are arity-flexible at the **value-last** end (`PAYLOAD` is the
last argument), matching the rest of the dialect, so you can just as
easily write the threaded form `(-> payload (json-get "temp") ...)`.

## Bars

`(bar WINDOW X)` is an OHLC bar accumulator for streams where you want
open / high / low / close summaries instead of every tick. `WINDOW` is
either `(ticks N)` (close every N samples) or `(window-ms N)` (close
every N ms of wall-clock time, aligned to `floor(now/N)*N`). Each call
folds one sample into the in-progress bar; on close the rule publishes
four sub-subjects under `<subject>.bar`:

| sub-subject  | value                                                     |
|--------------|-----------------------------------------------------------|
| `.bar.open`  | first sample of the bar                                   |
| `.bar.high`  | max sample seen in the bar                                |
| `.bar.low`   | min sample seen in the bar                                |
| `.bar.close` | last sample seen in the bar (the closing tick or last     |
|              | sample before the boundary, for time bars)                |

State is per `(rule, subject, window kind)`, so a single rule on
`MARKET.*` builds independent bars for every symbol, and a tick bar
plus a time bar on the same subject keep distinct slots. The op
returns `nil`, so it slots into a `do` block or stands alone as the
rule body without polluting a threaded pipeline. In-progress bars
survive a snapshot reload.

For tick bars, the close happens inline on the Nth sample. For time
bars, the close happens either on the next sample that crosses the
boundary, or on the server's clock walker tick if no further samples
arrive. The walker fires every ~500ms; expect close events to land
within roughly half its tick of the wall-clock boundary.

```edn
; 60-tick OHLC bars per symbol on a market feed.
(on "MARKET.*"
  (bar (ticks 60) payload-float))

; 1-minute aligned OHLC bars on the same feed. Bars cover wall-clock
; minutes (00:00..00:01, 00:01..00:02, ...) regardless of how many
; ticks land in each.
(on "MARKET.*"
  (bar (window-ms 60000) payload-float))
```

A subscriber to `MARKET.AAPL.bar.>` then sees a clean burst of four
messages per closed bar, in `open / high / low / close` order, with no
intermediate per-tick noise.

Volume isn't reported. For tick bars it's always exactly N; for time
bars, pair the rule with `(count)` if you want the per-bar tick count.

## Running counters

`(count)` is a side-effecting running counter, per `(rule, subject)`.
Each call increments and publishes the new total to `<subject>.count`.
Returns nil so it threads or sits in a `do` block without disturbing the
value being passed through.

With one optional argument, `(count COND)` only increments when `COND`
is truthy (any value type, same rules as `if` / `when`). The predicate
itself is fully general, so it composes with everything the dialect
already has (`contains?`, comparisons, `changed?`, `rising-edge`, ...).

```edn
; Total tick count per symbol, live on MARKET.AAPL.count etc.
(on "MARKET.*"
  (count))

; Error-event counter per service.
(on "events.>"
  (count (contains? payload "ERROR")))

; Threshold breaches.
(on "sensors.>"
  (count (> payload-float 100)))
```

State is a single number, snapshot-persisted, so a restart resumes from
the last seen total instead of zero. Subscribe to `<subject>.count` (or
`$LVC.<subject>.count` for the latest value) to watch a counter live.
