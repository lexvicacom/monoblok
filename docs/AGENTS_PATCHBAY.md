# Patchbay agent instructions

Paste the block below into an LLM coding agent's project instructions
whenever you want the model to author or edit patchbay rule files for
monoblok. It is self-contained: it does not assume the model has read
the repo.

## Install

For Codex and other agents that read `AGENTS.md`, append this file to
the project's agent instructions:

```sh
# project-scoped
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/docs/AGENTS_PATCHBAY.md >> ./AGENTS.md
```

Codex discovers repository-scoped `AGENTS.md` automatically when it
runs in that tree. For other tools, paste or reference this file in
the equivalent project instruction area: Cursor rules, Aider
conventions, ChatGPT project instructions, or another agent-specific
memory file. For one-off use, reference `docs/AGENTS_PATCHBAY.md` in
the prompt and ask the model to follow it while editing `.edn`
patchbay files.

Agent guidance
---

You are writing rules for **monoblok's patchbay**, a small s-expression
DSL that runs on top of a NATS-compatible message router. Rule files
live on disk (conventionally `patchbay.edn`) and are loaded at daemon
startup. Your output is always a valid patchbay file, no prose outside
EDN comments (`;` to end of line).

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

There are also top-level config forms: `(lvc ["filter" ...])` opts matching
subjects into `$LVC.*` last-value streams, and optional `(bridge ...)`
forwards local publishes to a remote NATS cluster. The bridge is export-only.

## Grammar (s-expressions)

`(head arg1 arg2 ...)`, no commas, no infix. Strings `"..."`, numbers
bare, `nil` / `true` / `false` are literals. Truthiness: only `nil` and
`false` are falsy (so `0` and `""` are truthy). Comments start with `;`.

Unlike Clojure, there is no quote form. Whether a `(...)` is a call
depends on context, not on a leading `'`. Inside an `(on ...)` body,
every list dispatches on its head symbol (`hold-off`, `publish`, `+`,
etc.) - unknown heads error. Inside the `(lvc ...)`, `(bridge ...)`, and `(mixer
...)` forms, after a keyword like `:servers`, `:export`, or
`:workers`, a list is a literal sequence of elements (e.g. servers to
try when connecting). Same parser, different consumer. The mixer is
documented in [`docs/mixer.md`](./mixer.md).

For unambiguous data inside a body, write a vector with square
brackets: `[1 2 3]`, `["red" "green" "blue"]`. Vectors self-evaluate
(elements eval'd in place, returned as a vector) and never dispatch on
a head, so they're how you spell a literal collection (it's what
`contains?` checks for membership against). Config readers (`bridge`,
`mixer`) accept either `(...)` or `[...]` for keyword-tagged
collections; vectors are the recommended form.

## Bound symbols (the current message)

- `subject` (string), `payload` (string bytes)
- `payload-float` (f64, errors if payload is not numeric)
- `payload-int` (signed int, errors on non-integer input)

## Operator cheat sheet

Special forms (lazy / short-circuiting):

- `(if C T E?)`, `(when C BODY...)`, `(do X...)`
- `(and X...)`, `(or X...)`, `(not X)`
- `(-> X f1 f2 ...)` threads `X` as the **last** arg of each `fN`
- `(transition C UP DOWN)` fires `UP` on false->true, `DOWN` on true->false, otherwise nil

Comparisons (chained, numeric coerce strings): `=` `<` `<=` `>` `>=`

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
  All three share one threadlocal buffer keyed by the minute, so any combination of granularities in a hot loop costs one integer compare and a slice (recompute only on a minute boundary). Wall-clock comes from `Context.wall_ms` (stamped per-PUB by the server, distinct from the monotonic `now_ms` used by `hold-off` and time windows). Pair with `subject-with` or `subject-append` for bucketing: `(publish! (subject-with "logs" (now :date) "errors") payload)` lands on `logs.2026-05-04.errors`. `:minute` is high cardinality (525,600 unique subject tokens per year per topic); prefer `:hour` or `:date` unless you really mean it.
- `(contains? coll item)` substring on strings (`(contains? payload "ERROR")`), membership on vectors (`(contains? [1 2 3] payload-int)`). Strict equality, so `"1"` does not match `1`.
- `(starts-with? text prefix)`, `(ends-with? text suffix)` strings only
- `(subject-token N [S])` Nth dot token, 0-indexed, of `S` (default: `subject`)

Side effects:

Forms whose entire purpose is to emit (terminal effect, return nil) carry a trailing `!`. Scanning a rule, the bangs are the lines that put bytes on the wire; everything else is pure or value-returning. Un-banged spellings (`publish`, `publish-to`, `publish-to!`, `json-demux`, `count`, `bar`) are still accepted as aliases. `publish` and `publish-to` were once distinct (args flipped); they collapsed to one form when the distinction stopped mattering. Use `publish!`.

- `(publish! SUBJECT VALUE)` validates SUBJECT, coerces VALUE (numbers stringified canonically, booleans -> "true"/"false"), enqueues. No-op if VALUE is nil so a suppressed gate upstream self-terminates the chain. Returns nil.

Idempotent filters (per-rule, per-subject state; first sight always passes / is treated as "no prior"):

- `(round N X)`, `(quantize STEP X)` pure
- `(squelch X)` pass iff X differs from last X, else nil
- `(deadband DELTA X)` pass iff numeric X moved by >= DELTA, else nil
- `(changed? X)` boolean predicate form of squelch
- `(delta X)` numeric difference since last X (0 on first sight)
- `(hold-off MS X)` pass X on first sight and again only after MS ms have elapsed since the last pass; nil otherwise. Time source is the server-stamped per-message clock, so all ops in one evaluation see the same "now".

Windows (used as the first arg(s) of windowed ops):

- bare integer `N`: last N samples; ring is fixed-cap, allocated once
- `:ms N`: last N ms of wall-clock time (ingress timestamp); samples evict by age, and the server arms one timer per active time-windowed slot so quiet streams don't keep stale data

Windowed aggregates (per `(rule, subject, op, window-kind)` slot; tick and time variants on the same rule + subject keep distinct state):

- `(moving-avg WINDOW X)`, `(moving-sum WINDOW X)`, `(moving-max WINDOW X)`, `(moving-min WINDOW X)`. WINDOW is `N` (ticks) or `:ms N` (time). Tick form is O(1) per update; time form is O(n) over the live window.
- `(rate :ms N X)` events per second. Tick form is rejected because rate needs a time unit. X is evaluated but ignored; the op counts pushes.
- `(percentile WINDOW P X)` Pth percentile (P in [0, 1]). `(median WINDOW X)` is sugar for `(percentile WINDOW 0.5 X)`. O(n log n) per call.
- `(stddev WINDOW X)`, `(variance WINDOW X)` population stddev / variance over WINDOW. O(n).
- `(throttle WINDOW MAX X)` pass X iff fewer than MAX events have already passed within WINDOW (then record this pass). Differs from `hold-off`: `hold-off` is min-interval-between-passes; `throttle` is max-count-per-window.
- `(on-silence :ms N BODY...)` clocked special form: reset a per-subject timer on each match; evaluate BODY with the last subject/payload if no match arrives for N ms.
- `(debounce! :ms N SUBJECT VALUE)` trailing-edge publish: emit the latest SUBJECT/VALUE after N ms of quiet.
- `(sample! :ms N SUBJECT VALUE)` cadence publish: emit the latest SUBJECT/VALUE every N ms after first match.
- `(aggregate! :ms N SUBJECT :METRIC X)` clocked aggregate publish. Metrics: `:avg`, `:sum`, `:min`, `:max`, `:count`, `:rate`.

Edge gates (take a boolean, fire once per transition; first sight never fires):

- `(rising-edge X)`, `(falling-edge X)`

JSON (top-level object only, no JSON path, value-last):

- `(json-get KEY PAYLOAD)` returns the field as number / string / boolean, or nil if missing, malformed, null, or nested. Threads through `->` and short-circuits `publish!` on nil.
- `(json-demux! KEY ... PAYLOAD)` side-effecting demux: for each KEY, publishes the field's value to `<subject>.<key>`. Skips missing / null / nested fields silently. Returns nil. Use as the head rule for JSON-emitting devices so the rest of the patchbay stays scalar.

Bars (side-effecting, per (rule, subject, window-kind)):

- `(bar! WINDOW X)` accumulates X into an in-progress bar. WINDOW is `N` (close every N samples) or `:ms N` (close every N ms of wall-clock time, aligned to `floor(now/N)*N`). On close, publishes `<subject>.bar.open`, `.high`, `.low`, `.close`. Returns nil. Volume isn't reported (N for tick bars, varies for time bars; pair with `(count!)` if you need it). Time bars also close from the slot's exact deadline timer if a window elapses without a new sample.

Running counters (side-effecting, per (rule, subject)):

- `(count!)` and `(count! COND)` increment a running total and publish it to `<subject>.count`. With no args, fires every call; with one arg, only when COND is truthy (same rules as `if` / `when`, so any predicate composes: `(count! (contains? payload "ERROR"))`, `(count! (> payload-float 100))`, etc.). State is a `.number`, snapshot-persisted. Returns nil so it threads or sits in a `do` block without disturbing the value flowing past.

- `(print! X)` and `(print! LABEL X)` are debug aids: write one line to stderr (`print! [SUBJECT] LABEL = VALUE`) and return `X` unchanged, so they sit cleanly inside `(-> ... (print! "raw") (round 1) (print! "rounded") ...)` without changing the value path. `print!` is not a publish (no `$STATS` bump, not gated by `--trace`). The loader walks every rule body and counts `print!` calls; if any are present the server logs a single warning at startup so a left-in `print!` is visible in the boot log. Use it during debugging, take it out when you ship.

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
- Assuming `first sight` fires an edge. `rising-edge` / `falling-edge` / `transition` all stay quiet until they have seen at least one prior value.

## Bridge form (optional, zero or one at top level)

```edn
(bridge
  :servers ["tls://a.example:4222" "tls://b.example:4222"]
  :creds   "/etc/monoblok/ngs.creds"
  :tls     true
  :name    "monoblok-prod-1"
  :export  ["telemetry.>" "alerts.>"])
```

Other keywords: `:user` / `:password`, `:token`, `:tls-ca`, `:tls-cert`
/ `:tls-key`, `:tls-skip-verify` (dev only), `:connect-timeout-ms`,
`:ping-interval-ms`, `:max-reconnect` (-1 unlimited), `:reconnect-wait-ms`.
`:export` is a vector of subject filters; matched publishes are
forwarded as-is. Nothing flows back. Both `:servers` and `:export`
also accept the legacy `(...)` list syntax, but `[...]` is the
recommended form. LVC follows the same recommendation:
`(lvc ["sensors.>" "alerts.>"])`, with legacy `(lvc "sensors.>")`
still accepted.

## Worked examples

```edn
; Fan high readings onto a dedicated sub-subject.
(on "sensors.*"
  (when (> payload-float 30.0)
    (publish! (subject-append "high") payload)))

; Mirror anything that looks like an alert onto events.alerts.
(on ">"
  (when (contains? payload "alert")
    (publish! "events.alerts" (str-concat subject ": " payload))))

; Clean up an RPM stream: 50rpm buckets, drop duplicates.
(on "car.*.rpm"
  (-> payload-float
      (quantize 50)
      (squelch)
      (publish! (subject-append "stable"))))

; Smooth + deadband in one pipeline.
(on "sensors.*"
  (-> payload-float
      (moving-avg 10)
      (deadband 1.0)
      (publish! (subject-append "smoothed"))))

; Time-windowed smoothing: 5-second sliding average. Old samples drop
; off on the next push or on the slot's deadline timer.
(on "sensors.*"
  (-> payload-float
      (moving-avg :ms 5000)
      (publish! (subject-append "avg5s"))))

; Clocked liveness + trailing/cadence values.
(on "devices.*.heartbeat"
  (on-silence :ms 30000
    (publish! (subject-append "stale") "true")))

(on "knobs.*"
  (debounce! :ms 250 (subject-append "settled") payload))

(on "sensors.*"
  (sample! :ms 1000 (subject-append "sampled") payload))

(on "sensors.*"
  (aggregate! :ms 5000 (subject-append "avg5s") :avg payload-float))

; Sustained-heat alert and all-clear, one rule, one ring.
(on "temp.*.*"
  (transition (> (moving-avg 60 payload-float) 28.0)
    (publish! (subject-append "alert") "hot")
    (publish! (subject-append "ok")    "cool")))

; Volatility: two windows, non-threaded form.
(on "sensors.*"
  (when (> (- (moving-max 20 payload-float)
              (moving-min 20 payload-float)) 5.0)
    (publish! (subject-append "volatile") payload)))

; 1-minute aligned OHLC bars on a market feed. Bars close on the next
; tick that crosses a wall-clock minute boundary, or from the slot's
; deadline timer if the feed goes quiet.
(on "MARKET.*"
  (bar! :ms 60000 payload-float))

; Live events/sec per subject, useful for monitoring dashboards.
(on "events.>"
  (-> payload (rate :ms 1000) (publish! (subject-append "hz"))))

; Tail-latency monitor: only flag rules where p99 over the last 200
; samples exceeds a threshold.
(on "rpc.latency.*"
  (when (> (percentile 200 0.99 payload-float) 250.0)
    (publish! (subject-append "slow") payload)))

; At most 5 alerts per minute, regardless of how they arrive.
(on "alerts.>"
  (-> payload
      (throttle :ms 60000 5)
      (publish! (subject-append "rate-limited"))))
```

## Authoring checklist

Before returning a patchbay file, verify:

1. Every top-level form is `(on ...)`, `(lvc ...)`, or a single optional `(bridge ...)`.
2. Every `publish!` target is a concrete subject (no `*` or `>`, no `$LVC.` or `$STATS.` prefix - those are read-only).
3. Every numeric op is fed a numeric value (`payload-float` / `payload-int` / arithmetic result), not raw `payload`.
4. `N` in `moving-*` is a literal integer and consistent per call site.
5. Pipelines built with `->` end in a side-effecting form (`publish!`); a pipeline that ends in a pure value is dead code.
6. Comments start with `;` and never leak prose outside them.
