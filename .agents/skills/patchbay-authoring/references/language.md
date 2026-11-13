# Patchbay language

## Mental model

The router receives NATS publishes. For each publish, every top-level
`(on FILTER BODY)` whose `FILTER` matches the incoming subject runs its
`BODY` against that message. Subject wildcards: `*` matches one dot
token, `>` matches one or more trailing tokens.

By default, messages published **from** a rule go out through normal
fan-out but do **not** re-enter rule evaluation, so chains don't form
unless you ask for them. Opt a rule into re-entry with `(on FILTER
:reentrant true BODY)`: its emissions then feed back through every
matching rule (including itself), capped at depth 8 so a rule whose
emission matches its own filter can't loop forever. Reach for it when
you genuinely want a multi-stage pipeline (e.g. `json-demux!` head rule
emitting scalars that downstream rules condition); otherwise leave it
off.

Monoblok also has a top-level `(every :ms N [:reentrant BOOL] BODY...)` form that
runs BODY on a fixed period instead of on a publish (see [Monoblok features](monoblok.md#heartbeats-every)).

In full monoblok there are also top-level config forms: `(lvc ["filter"
...])` opts matching subjects into `$LVC.*` last-value streams, and optional
`(export ...)` forwards local publishes to a remote NATS cluster. Deprecated
`(bridge ...)` is accepted as a compatibility alias. Optional
`(import ...)` subscribes to a remote NATS cluster (core subjects, JetStream
durable consumers, or both) and feeds matching messages into patchbay as
private ingress. Imported raw subjects are not visible to direct monoblok
subscribers unless a rule republishes them explicitly.

## Grammar (s-expressions)

`(head arg1 arg2 ...)`, no commas, no infix. Strings `"..."`, numbers
bare, `nil` / `true` / `false` are literals. Truthiness: only `nil` and
`false` are falsy (so `0` and `""` are truthy). Comments start with `;`.

Unlike Clojure, there is no quote form. Whether a `(...)` is a call
depends on context, not on a leading `'`. Inside an `(on ...)` body,
every list dispatches on its head symbol (`hold-off`, `publish`, `+`,
etc.) - unknown heads error. Inside the `(lvc ...)`, `(export ...)`,
deprecated `(bridge ...)`, and `(import ...)` forms, after a keyword like
`:servers`, `:export`, or `:subject`, a list is a literal sequence of elements (e.g. servers to
try when connecting). The exception is `(env "NAME")` in top-level config
string positions; that is a load-time environment lookup, not an evaluator
form. Same parser, different consumer.

For unambiguous data inside a body, write a vector with square
brackets: `[1 2 3]`, `["red" "green" "blue"]`. Vectors self-evaluate
(elements eval'd in place, returned as a vector) and never dispatch on
a head, so they're how you spell a literal collection (it's what
`contains?` checks for membership against). Config readers (`export`,
deprecated `bridge`, `import`) accept either `(...)` or `[...]` for keyword-tagged
collections; vectors are the recommended form.

## Bound symbols (the current message)

- `subject` (string), `payload` (string bytes)
- `payload-float` (f64, errors if payload is not numeric)
- `payload-int` (signed int, errors on non-integer input)
- `payload-bool` (`true`/`1`/`on` is true, `false`/`0`/`off` is false, ASCII
  case-insensitive; anything else is a type error)
- `reply-to` (the request's reply subject, or nil when the message had none)
- `replaying?` (true while JetStream catch-up is evaluating historical
  messages, false once the listener is open and for every non-JetStream
  message)

An empty payload is not the number 0: `payload-float`, `payload-int`, and
`payload-bool` on an empty payload are type errors. Unknown forms and unknown
symbols are rejected at load and by `--validate`, in every branch.

## Operator cheat sheet

Special forms (lazy / short-circuiting):

- `(if C T E?)`, `(when C BODY...)`, `(do X...)`
- `(and X...)`, `(or X...)`, `(not X)`
- `(-> X f1 f2 ...)` threads `X` as the **last** arg of each `fN`
- `(transition C UP DOWN)` fires `UP` on false->true, `DOWN` on true->false, otherwise nil

Comparisons: `=` is tag-strict; `<` `<=` `>` `>=` are chained and coerce numeric strings.

Arithmetic (variadic, left-fold): `+` `-` `*` `/`, plus `min`, `max`,
`abs`, `sign`, `(clamp LO HI X)` (value last so it threads).

Strings / subjects:

- `(str-concat a b ...)`
- `(subject-append "suffix")` produces `"<subject>.suffix"`
- `(subject-with TOK ...)` / `(subject-with [TOK ...])` joins tokens with `.` to build a publishable subject. Numbers and bools coerce to strings; empty tokens raise `InvalidSubject`. Result is publish-validated, so `(publish! (subject-with "rooms" room-id "temp") payload)` either fires a valid subject or errors at construction. Use this when `subject-append` doesn't fit (you're not extending the current subject).
- `(now :date)` / `(now :hour)` / `(now :minute)` returns a UTC wall-clock string at the chosen granularity:
  - `:date` -> `"YYYY-MM-DD"` (10 chars)
  - `:hour` -> `"YYYY-MM-DDTHH"` (13 chars, RFC 3339 `T` separator)
  - `:minute` -> `"YYYY-MM-DDTHHMM"` (15 chars, RFC 3339 basic form, no `:` so it's subject-token-safe)
  Each call formats the result into the eval arena. Wall-clock comes from `Context.wall_ms` (stamped per-PUB by the server, distinct from the monotonic `now_ms` used by `hold-off` and time windows). Pair with `subject-with` or `subject-append` for bucketing: `(publish! (subject-with "logs" (now :date) "errors") payload)` lands on `logs.2026-05-04.errors`. `:minute` is high cardinality (525,600 unique subject tokens per year per topic); prefer `:hour` or `:date` unless you really mean it.
- `(contains? coll item)` substring on strings (`(contains? payload "ERROR")`), membership on vectors (`(contains? [1 2 3] payload-int)`). Strict equality, so `"1"` does not match `1`.
- `(starts-with? text prefix)`, `(ends-with? text suffix)` strings only
- `(subject-token N [S])` Nth dot token, 0-indexed, of `S` (default: `subject`)

Side effects:

Forms whose entire purpose is to emit (terminal effect, return nil) carry a trailing `!`. Scanning a rule, the bangs are the lines that put bytes on the wire; everything else is pure or value-returning. Un-banged spellings (`publish`, `publish-to`, `publish-to!`, `json-demux`, `count`, `bar`) are still accepted as aliases. `publish` and `publish-to` were once distinct (args flipped); they collapsed to one form when the distinction stopped mattering. Use `publish!`.

- `(publish! SUBJECT VALUE)` validates SUBJECT, coerces VALUE (numbers stringified canonically, booleans -> "true"/"false"), enqueues. `(publish! SUBJECT :dp N VALUE)` formats numeric VALUE with exactly N decimal places at the output boundary, with N in 0-15; in a thread use `(-> payload-float (moving-avg 10) (publish! "out.avg" :dp 1))`. No-op if VALUE is nil so a suppressed gate upstream self-terminates the chain. Returns nil.

Idempotent filters (state is per call site and subject, so two uses of the same form in one rule keep separate state; first sight always passes / is treated as "no prior"):

- `(round N X)`, `(quantize STEP X)` pure
- `(squelch X)` pass iff X differs from last X, else nil
- `(deadband DELTA X)` pass iff numeric X moved by >= DELTA, else nil
- `(changed? X)` boolean predicate form of squelch
- `(delta X)` numeric difference since last X (0 on first sight)
- `(hold-off MS X)` pass X on first sight and again only after MS ms have elapsed since the last pass; nil otherwise. Time source is the server-stamped per-message clock, so all ops in one evaluation see the same "now".

Windows (used as the first arg(s) of windowed ops):

- bare integer `N`: last N samples; ring is fixed-cap, allocated once
- `:ms N`: last N ms of wall-clock time (ingress timestamp); samples evict by age, active slots expose deadlines, and the server keeps one rescheduled timer pointed at the earliest active deadline so quiet streams don't keep stale data

Windowed aggregates (per call site, subject, and window kind; tick and time variants keep distinct state):

- `(moving-avg WINDOW X)`, `(moving-sum WINDOW X)`, `(moving-max WINDOW X)`, `(moving-min WINDOW X)`. WINDOW is `N` (ticks) or `:ms N` (time). Tick form is O(1) per update; time form is O(n) over the live window.
- `(ewma ALPHA X)` exponentially weighted average, 0 < ALPHA <= 1. First sample returns X, then `ALPHA*X + (1-ALPHA)*prev`. One number of state, no ring, so prefer it over a big `moving-avg` when you only want smoothing.
- `(rate :ms N X)` events per second. Tick form is rejected because rate needs a time unit. X is evaluated but ignored; the op counts pushes.
- `(percentile WINDOW P X)` Pth percentile (P in [0, 1]). `(median WINDOW X)` is sugar for `(percentile WINDOW 0.5 X)`. O(n log n) per call.
- `(stddev WINDOW X)`, `(variance WINDOW X)` population stddev / variance over WINDOW. O(n).
- `(throttle WINDOW MAX X)` pass X iff fewer than MAX events have already passed within WINDOW (then record this pass). Differs from `hold-off`: `hold-off` is min-interval-between-passes; `throttle` is max-count-per-window.
- `(on-silence :ms N BODY...)` clocked special form: reset a per-subject timer on each match; evaluate BODY with the last subject/payload if no match arrives for N ms.
- `(dropout :ms N :lost LOST :found FOUND)` clocked liveness latch: first match arms the timer quietly; silence evaluates LOST once; the next match after that evaluates FOUND once.
- `(debounce! :ms N SUBJECT VALUE)` trailing-edge publish: emit the latest SUBJECT/VALUE after N ms of quiet.
- `(sample! :ms N SUBJECT VALUE)` cadence publish: emit the latest SUBJECT/VALUE every N ms after first match.
- `(aggregate! :ms N SUBJECT :METRIC X)` clocked aggregate publish. Metrics: `:avg`, `:sum`, `:min`, `:max`, `:count`, `:rate`.

Edge gates (take a boolean, fire once per transition; first sight never fires):

- `(rising-edge X)`, `(falling-edge X)`

Hysteresis (stateful, returns a bool on every call):

- `(hysteresis LO HI X)` true once X >= HI, false once X <= LO, otherwise the previous answer. First sight is `X >= HI`. `LO > HI` or a non-numeric argument is a type error. Feed it to `transition` to get an alert and all-clear that do not chatter around one threshold: `(transition (hysteresis 26.0 28.0 payload-float) (publish! ... "hot") (publish! ... "ok"))`.

JSON (value-last):

- `(json-get KEY PAYLOAD)` returns the selected top-level key or dotted object path up to four tokens deep (`"a.b.c.d"`) as number / string / boolean, or nil if missing, malformed, null, array, or non-selected nested object. Threads through `->` and short-circuits `publish!` on nil.
- `(json-demux! KEY ... PAYLOAD)` side-effecting demux: for each top-level KEY or dotted object path up to four tokens deep (`"a.b.c.d"`), publishes the value to `<subject>.<suffix>`. By default suffix is the key/path; `:leaf` uses only the final path token, and `[PATH SUFFIX]` overrides one output suffix. Skips missing / null / arrays / non-selected nested objects silently. Returns nil. Use as the head rule for JSON-emitting devices so the rest of the patchbay stays scalar.

Bars (side-effecting, per call site, subject, and window kind):

- `(bar! WINDOW X)` accumulates X into an in-progress bar. WINDOW is `N` (close every N samples) or `:ms N` (close every N ms of wall-clock time, aligned to `floor(now/N)*N`). On close, publishes `<subject>.bar.open`, `.high`, `.low`, `.close`. Returns nil. Volume isn't reported (N for tick bars, varies for time bars; pair with `(count!)` if you need it). Time bars also close from the slot's exact deadline timer if a window elapses without a new sample.

Running counters (side-effecting, per call site and subject):

- `(count!)` and `(count! COND)` increment a running total and publish it to `<subject>.count`. Use `(count! :subject SUBJECT)` or `(count! :subject SUBJECT COND)` to publish that counter to an explicit subject; the state remains per source call site and subject. With no condition, fires every call; with COND, only when COND is truthy (same rules as `if` / `when`, so any predicate composes: `(count! (contains? payload "ERROR"))`, `(count! (> payload-float 100))`, etc.). State is a `.number`, snapshot-persisted. Returns nil so it threads or sits in a `do` block without disturbing the value flowing past.

- `(print! X)` and `(print! LABEL X)` are debug aids: write one line to stderr (`print! [SUBJECT] LABEL = VALUE`) and return `X` unchanged, so they sit cleanly inside `(-> ... (print! "raw") (round 1) (print! "rounded") ...)` without changing the value path. `print!` is not a publish (no `$STATS` bump, not gated by `--trace`). The loader walks every rule body and counts `print!` calls; if any are present the server logs a single warning at startup so a left-in `print!` is visible in the boot log. Use it during debugging, take it out when you ship.

## Funnel: combining subjects

State is per subject. There is no shared state across subjects and no key
form. To aggregate across subjects, publish them onto one subject with a
`:reentrant true` rule, and put the stateful rule on that subject. The funnel
subject must not match the source filter:

```edn
(on "sensors.*" :reentrant true
  (publish! "all.sensors" payload))

(on "all.sensors"
  (publish! "sensors.average" :dp 1 (moving-avg 4 payload-float)))
```

Per group, build the funnel subject from a token and match it with a wildcard:

```edn
(on "site.*.*" :reentrant true
  (publish! (subject-with "group" (subject-token 1)) payload))

(on "group.*"
  (publish! (subject-append "average") :dp 1 (moving-avg 8 payload-float)))
```

## Gating: deciding what is forwarded

A rule can be a gate instead of a transform: publish the message onward under
a different prefix only when it passes, and publish a verdict elsewhere when it
does not. The gate is only real when the raw subjects have no other way out,
so pair it with `(export :export ["clean.>" ...])` (publishers connect to
monoblok, only the clean prefix reaches the cluster) or with `(import ...)`
(raw input is private). Rules see subject and payload only, never which
connection published, so every decision is per subject, not per client.

```edn
; Payload scan. Report the subject, not the offending payload.
(on "in.event.*"
  (if (or (contains? payload "<script") (contains? payload "DROP TABLE"))
    (publish! "waf.blocked" subject)
    (publish! (subject-with "clean.event" (subject-token 2)) payload)))

; Outlier gate: forward only values at or below the rolling p99.
(on "in.reading.*"
  (when (<= payload-float (percentile 100 0.99 payload-float))
    (publish! (subject-with "clean.reading" (subject-token 2)) payload)))

; Flood verdict: one "block" on the way up, one "clear" on the way down.
(on "in.*.*"
  (transition (> (rate :ms 5000 payload) 2)
    (publish! (subject-with "waf.flood" subject) "block")
    (publish! (subject-with "waf.flood" subject) "clear")))

; De-identify by allow-list: name the fields that may leave and drop the
; identifying subject token. Fields that are not named are never forwarded.
(on "ward.*.*"
  (-> payload (json-get "hr") (publish! (subject-with "clean" (subject-token 1) "hr"))))
```

Things to tell the user when you write gates like these:

- The percentile window includes the current sample, so a value that sets a
  new window maximum is always held back (about 1 in 100 on a steady stream,
  every message on a stream that only climbs), and held-back samples still
  count towards later percentiles. Add a fixed ceiling with `and` when there
  is a hard limit.
- `transition` only runs when a message arrives, so the "clear" is published
  on the first message after the rate has dropped, not when the window empties.
  The verdict does not stop the traffic; use `throttle` in the forwarding
  thread to cap it.
- `contains?` is a case-sensitive substring test with no decoding.
- Prefer an allow-list of fields over hunting for bad values: it needs no
  pattern matching and fails closed when a new field appears.

## Forms that do not exist

Patchbay has no regular expressions, no string replace, substring, split,
upper/lower or length, no hashing, tokenising or masking, no JSON
construction or field deletion, no arrays or JSON paths deeper than four
tokens, no variables, `let`, user-defined functions, macros or quote, and no
state shared across subjects. Do not invent a form to fill one of these gaps:
an unknown form is a load error. Say what cannot be expressed and suggest
doing it upstream (in the publisher or gateway) or downstream instead.

## The `->` pipeline idiom

Because every transform / gate takes its value last, `->` reads
top-to-bottom as a data pipeline: each step receives the previous
step's result as its final argument.

```edn
(-> x f1 f2)
```

means:

```edn
(f2 (f1 x))
```

```edn
(-> payload-float
    (round 1)
    (squelch)
    (publish! (subject-append "stable")))
```

Gates return `nil` on suppress; `publish!` is a no-op on `nil`. That
is how a single pipeline "round, dedupe, emit" works without a `when`.

## Idioms to prefer

- For "dedupe after rounding / quantizing", thread `round` or `quantize` into `squelch` into `publish!`. Don't hand-roll a last-value check.
- For analog noise, reach for `deadband` before `squelch`; for slow drift, wrap `moving-avg` in `deadband`.
- For a threshold that chatters, put `hysteresis` inside `transition` rather than comparing against one number.
- To combine several subjects into one aggregate, use the funnel pattern; do not look for shared state.
- For a join of two subjects, use `last` in a thread (nil-safe), with `(lvc ...)` covering the other subject.
- For alert + all-clear pairs on one predicate, use `transition`, not two `rising-edge` / `falling-edge` rules. It shares the ring and the prev slot.
- For one-sided alerts that still need to thread, use `rising-edge` / `falling-edge`.
- When you need multiple windows on the same stream (e.g. max - min spread), drop out of `->` and write the explicit nested form. `->` only threads one value.
- Keep subjects hierarchical: emit into `<input-subject>.<suffix>` via `subject-append` so downstream subscribers can pick the granularity they want.
- Don't publish back into a subject your own rule matches unless you have explicitly thought about it. Rules without `:reentrant true` don't loop, but adding it on a rule whose emission can match its own filter (or another `:reentrant` rule's filter) is how you build cascades. Keep the depth cap (8) in mind and prefer non-reentrant unless you genuinely need staging.
- For sensors that emit JSON frames, prefer `json-demux!` at the head of the rule chain to fan fields out to scalar sub-subjects, then write the rest of your patchbay against those scalars. Reach for `json-get` inline only when you genuinely want one field on the existing subject.

## Anti-patterns

- Re-implementing `squelch` / `deadband` with `if` and a hand-rolled "last value" (there is no way to store state outside the gate primitives; the gates ARE the state).
- Using `=` to compare a number and a string. `=` is tag-strict; compare with `<` / `>` or parse first.
- Changing `N` in `(moving-avg N ...)` between invocations on the same rule. The first call's `N` wins for the lifetime of the slot.
- Trying to share state across subjects. State is per subject; use the funnel pattern.
- Doing arithmetic or comparison on `(last ...)` without a nil guard or a `->` thread.
- Writing `reply!` in a rule and testing it under `--soundcheck`; `reply-to` is always nil there, so it publishes nothing. Check request-reply with a real `nats req`.
- Writing bare `payload` (or `:ms`, or an unquoted word) in YAML, or bare `"payload"` in JSON. Use `$payload`, `ms`, quoted strings, and `"$payload"`.
- Assuming `first sight` fires an edge. `rising-edge` / `falling-edge` / `transition` all stay quiet until they have seen at least one prior value.
