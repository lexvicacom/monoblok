# patchbay reference

The [README](./README.md) covers what patchbay is and what it's for.
This doc is the full reference for the DSL: syntax, bound symbols, and
every operator. For the 30-second pitch and worked examples, start
there; for "what does `(deadband ...)` actually do," this is the page.
For one-line summaries of every form, see the
[cheatsheet](./patchbay-cheatsheet.md). Runnable patchbay files for
common scenarios live in [`examples/`](../examples/).

## S-expression syntax in 30 seconds

If the parens look alien: every list `(head arg1 arg2 ...)` is a call
where `head` is the operator and the rest are its arguments. No commas,
no infix, no precedence to memorise; `(+ 1 2 3)` is `1 + 2 + 3` and
`(> x 10)` is `x > 10`. Nesting is just a list inside a list:
`(publish! (subject-append "high") payload)` calls `publish!` with two
args, the first of which is itself the result of calling
`subject-append`. Strings are double-quoted, numbers bare, and
everything else (`subject`, `payload`, `when`, `+`) is a symbol
resolved by the evaluator.

That's the whole grammar. The rest of this doc is just which
operators exist and what they do.

### JSON patchbay files

EDN is the canonical notation for hand-written patchbays. Files ending
in `.json` are also accepted, but treat that as a compatibility layer:
useful for generated patchbays, web UIs, config systems, and users who
need JSON as an affordance. If you're editing rules directly, consider
embracing the S-expression form. It is terser, supports comments, and
keeps symbols distinct from strings without escape hatches. VS Code's
[Calva](https://calva.io/) gives good Clojure/EDN highlighting, bracket
matching, paren coloring, and Parinfer-style editing. In Emacs,
`clojure-mode`/CIDER works well; `clojure-ts-mode` is the newer
tree-sitter option. Parinfer, paredit, and smartparens are all good ways
to make S-expressions feel less manual.

JSON is read into the same patchbay AST, not a separate DSL: arrays are
forms and the first array item is always the operator symbol. In
argument positions, plain objects become keyword options, and the bound
names `"subject"`, `"payload"`, `"payload-float"`, `"payload-int"`, and
`"replaying?"` become symbols in rule expressions.

```json
[
  ["on", "sensors.*",
    ["->", "payload-float", ["round", 1], ["squelch"], ["publish!", ["subject-append", "stable"]]]],

  ["on", "market.*", {"reentrant": true},
    ["bar!", 60, "payload-float"]],

  ["export", {
    "servers": ["nats://127.0.0.1:4223"],
    "export": ["sensors.*.stable"]
  }]
]
```

Other strings in argument positions are string literals. Use
`{"str":"payload"}` if you really need the literal string `"payload"` in
a rule body, `{"kw":"ms"}` for a standalone keyword, and `{"vec":[...]}`
when a rule body needs a literal vector. See
[`examples/demo.json`](../examples/demo.json) for a fuller conversion of
[`examples/demo.edn`](../examples/demo.edn).

### YAML sugar files

Files ending in `.yml` or `.yaml` use a deliberately small patchbay
YAML subset. It is also lowered directly into the same patchbay AST:
YAML never evaluates and no EDN/JSON text is generated internally.
Supported shapes are block maps/lists, flow arrays for call forms,
quoted or unquoted scalars, numbers, booleans, nulls, top-level `on`,
`lvc`, `export`, `bridge`, and `import`. In top-level config string
positions, `env: NAME` lowers to `(env "NAME")`.

```yaml
on:
  - sub: car.*.rpm
    thread:
      from: payload-float
      steps:
        - [quantize, 50]
        - [squelch]
        - [publish!, [subject-append, "stable"]]
```

That lowers to the same AST as:

```edn
(on "car.*.rpm"
  (-> payload-float
      (quantize 50)
      (squelch)
      (publish! (subject-append "stable"))))
```

Within expression arrays, the first item is a form symbol; bound names
such as `payload`, `payload-float`, `payload-int`, `subject`, and
`replaying?` lower as symbols; other scalar arguments lower as strings
unless they are numbers, booleans, nulls, or keywords like `:ms`. See
[`examples/rental-car.yml`](../examples/rental-car.yml) for the fuller
shape and [`patchbay-yaml-schema.md`](./patchbay-yaml-schema.md) for the
YAML shape reference.

### EDN is the notation; the patchbay is the evaluator

Patchbay files are valid EDN, which is why `.edn` editor tooling
(syntax highlighting, paren-matching, structural editing) works out
of the box. EDN itself is a data notation — it parses lists `(...)`,
vectors `[...]`, strings, numbers, keywords, and so on, and stops
there. It does not say what any of those mean. The patchbay is the
piece that gives some of those forms call semantics, and only in
some places.

If you've used Clojure, this is the part where the muscle memory
diverges. Clojure's rule is "every `(...)` is a call unless I
`'`-quote it." Patchbay has no quote form and doesn't need one,
because whether a list dispatches as a call is decided by *where it
appears*, not by a sigil:

- Inside an `(on FILTER BODY)` body, every list dispatches on its
  head symbol (`hold-off`, `publish!`, `+`, etc.). Unknown heads
  error.
- Inside top-level config forms, sequences are data. In `(lvc ...)`,
  a single vector is read as the filter set. Inside
  `(export ...)`, deprecated `(bridge ...)`, and `(import ...)`, after a keyword like
  `:servers`, `:export`, or `:subject`, a list is read as a
  literal sequence of values. `(:servers ("nats://a:4222" "nats://b:4222") ...)` is a
  two-element list of strings, not a call to `nats://a:4222`. Same
  parser, different consumer. The one special list in config string
  positions is `(env "NAME")`, which reads an environment variable at
  patchbay load time.

For unambiguous data-as-data inside a rule body, write a vector with
square brackets: `[1 2 3]`, `["red" "green" "blue"]`. Vectors
self-evaluate (each element is evaluated, the result is returned as
a vector), they never dispatch on a head, and they're what
`contains?` checks for membership against. Config readers (`export`,
deprecated `bridge`, `import`) accept either `(...)` or `[...]` for keyword-tagged
collections; vectors read more naturally and are the recommended
form.

## NATS export/import config

`(export ...)` exports monoblok publishes to a real NATS cluster.
The old `(bridge ...)` form is deprecated but still accepted as a
compatibility alias:

```edn
(export
  :servers ["nats://127.0.0.1:4223"]
  :name    "monoblok-export"
  :export  ["sensors.*.stable" "alerts.>"])
```

`(import ...)` subscribes to subjects on a real NATS cluster and feeds
those messages into patchbay as ingress:

```edn
(import
  :servers ["nats://127.0.0.1:4223"]
  :name    "monoblok-import"
  :subject ["raw.>" "replay.>"]
  :max-pending 4096)

(on "raw.temp"
  (publish! "clean.temp" payload))
```

The flat form above is a compatibility alias for one core NATS import. Newer
configs can make the import kind explicit with a `:core` vector:

```edn
(import
  :core
  [[:servers ["nats://127.0.0.1:4223"]
    :name    "monoblok-import"
    :subject ["raw.>"]
    :max-pending 4096]])
```

The nested shape also accepts `:streams` entries for JetStream ingress. In v1,
each stream entry uses one subject filter so the loader can map it directly to
one JetStream filtered durable consumer.

```yaml
lvc:
  - "js.>"

import:
  streams:
    - servers:
        - env: JS_URL
      subject: "js.sensors.temp"
      stream: SENSORS
      consumer: monoblok-jetstream-example
      catch-up: true

on:
  - sub: js.sensors.temp
    form:
      - do
      - [count!]
      - [publish!, "js.metrics.avg20", [round, 2, [moving-avg, 20, payload-float]]]
      - [if, replaying?, [publish!, "js.replay.last-temp", payload], [publish!, "js.live.temp", payload]]
```

JetStream import is consumer-only: monoblok does not serve JetStream to local
clients. On startup, `:catch-up true` replays each configured stream entry to
the stream's startup high-water sequence before monoblok opens its listener.
Multiple stream entries catch up serially in config order. After catch-up, the
same durable consumers continue in live mode. `:catch-up false` skips the
historical event-time loading phase and consumes any durable backlog as live
processing-time work.

During JetStream catch-up, rules see `replaying?` as true. After the listener
opens, `replaying?` is false. Rules that do not mention `replaying?` produce
the same subjects during replay and live operation; rules that want to suppress
or redirect historical output should gate that behavior explicitly. See
[`examples/jetstream.yml`](../examples/jetstream.yml) and
[`examples/jetstream.sh`](../examples/jetstream.sh) for a runnable example that
starts JetStream on `JS_PORT` (default `15889`), exports `JS_URL`, populates
`COUNT=1000` historical events, and then runs monoblok through catch-up plus
one live event.

When `(import ...)` is present, monoblok's local NATS socket remains open for
`SUB`, `UNSUB`, `PING`, and LVC/stats reads, but client `PUB` commands are
rejected. Imported messages are private inputs. A direct monoblok subscriber to
`raw.>` will not see the imported `raw.temp`; subscribers only see subjects
emitted explicitly by rules, such as `clean.temp` above.

Core import entries accept the same connection keywords: `:creds`, `:user` /
`:password`, `:token`, `:tls`, `:tls-ca`, `:tls-cert` / `:tls-key`,
`:tls-skip-verify` (dev only), `:connect-timeout-ms`,
`:ping-interval-ms`, `:max-reconnect` (-1 unlimited), and
`:reconnect-wait-ms`. For import, `:origin-header true` ignores
messages carrying monoblok's `x-monoblok` header, useful when export
and import touch the same cluster. `:max-pending` bounds the
cross-thread import queue; the default is 4096 messages.

JetStream stream entries use the same connection/auth keywords plus `:stream`,
`:consumer`, and `:catch-up`. Event time comes from JetStream message metadata.
Malformed subjects, oversized payloads, missing metadata, failed rule
evaluation, or failed downstream publish prevent acking the JetStream message;
JetStream owns redelivery.

Top-level config string values may be written as `(env "NAME")`.
This is load-time only, not a rule-body form, and it reads exactly one
environment variable as one string. Missing or empty variables fail the
same non-empty validation as `""`; values are not split on commas:

```edn
(export
  :servers (env "NATS_SERVERS")
  :token   (env "NATS_TOKEN")
  :export  ["sensors.>"])
```

In YAML config, write the same value as `env: NAME`:

```yaml
export:
  servers:
    env: NATS_SERVERS
  token:
    env: NATS_TOKEN
  export: [sensors.>]
```

## Values

Patchbay values have a small set of runtime kinds: `nil`, booleans
(`true` / `false`), numbers (stored as C `double` values), strings
(`"..."`), symbols, lists `(...)`, and vectors `[...]`.
Truthiness: `nil` and `false` are falsy, everything else (including
`0`, `""`, and an empty vector) is truthy.

## Bound symbols

Bare symbols inside a body that resolve against the current message:

| symbol          | value kind | value                              |
|-----------------|------------|------------------------------------|
| `subject`       | string     | the incoming subject               |
| `payload`       | string     | the raw payload bytes              |
| `payload-float` | number     | `payload` parsed as a floating-point number (errors if not numeric) |
| `payload-int`   | number     | `payload` parsed as an integer and returned as a number (errors on non-integer input) |
| `replaying?`    | bool       | true while JetStream loading-phase replay is evaluating historical messages |

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
| `(dropout :ms N :lost L :found F)` | eval `L` after silence, `F` on recovery after a trip   |

## Comparisons and logic

All comparisons are chained: `(< a b c)` means `a < b && b < c`.
Numeric comparisons coerce string args by parsing them as numbers; `=`
only matches values of the same kind (a `number` never equals a
`string`).

| form              | notes                                                    |
|-------------------|----------------------------------------------------------|
| `(= a b ...)`     | all equal; same kind, deep-equal for strings/symbols     |
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
| `(subject-with TOK ...)` / `(subject-with [TOK ...])` | join tokens with `.` to build a publishable subject (e.g. `(subject-with "sensors" room "temp")`). Numbers and bools coerce to strings; empty tokens error. Result is publish-validated. |
| `(now :date)` / `(now :hour)` / `(now :minute)` | wall-clock UTC string at the chosen granularity: `"YYYY-MM-DD"`, `"YYYY-MM-DDTHH"`, `"YYYY-MM-DDTHHMM"` (RFC 3339 basic form, no `:` so it's subject-token-safe). The result is formatted into the eval arena for each call. `:minute` is high cardinality (525,600 unique values per year per topic) when used as a subject token, prefer `:hour` or `:date` unless you really mean it. |
| `(contains? coll item)`          | substring check on strings (`(contains? payload "ERROR")`), membership check on vectors (`(contains? [1 2 3] payload-int)`, `(contains? ["red" "green"] payload)`). Strict equality, so `"1"` does not match `1`. |
| `(starts-with? text prefix)`     | boolean prefix check (strings only)                 |
| `(ends-with? text suffix)`       | boolean suffix check (strings only)                 |
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
`foo.a.b.c.d`. `foo.*` matches `foo.a` only (a filter with N tokens
and no `>` requires exactly N tokens in the subject). If you need
"anything starting with `bip` with at least three tokens after it,"
that's `bip.*.*.*` or `bip.*.*.*.>`, not `bip.>` (which matches
`bip.a` too). Putting `>` anywhere except the tail is a validation
error, not a silent reinterpretation.

## Side effects

Forms whose entire purpose is to emit (terminal effect, return nil)
carry a trailing `!`. Scan a rule and the bangs are the lines that put
bytes on the wire; everything else is pure or value-returning.
Un-banged spellings (`publish`, `publish-to`, `publish-to!`,
`json-demux`, `count`, `bar`) still work as aliases for now.

A rule body may run more than one effect. Wrap them in `do` when the
same inbound message should trigger several side effects:

```edn
(on "MARKET.*"
  (do
    (publish! (subject-append "raw") payload)
    (bar! 60 payload-float)))
```

`(publish! SUBJECT VALUE)` validates `SUBJECT` as a publishable
subject (no wildcards, no `$LVC.*`), coerces `VALUE` to its canonical
string form (numbers stringified, booleans → `"true"` / `"false"`,
strings passed through), and enqueues a fan-out. Returns `nil`.
Publishes from rules participate in normal delivery + configured LVC caching. By
default they are not fed back through rule evaluation; opt in per rule
with `:reentrant true` (see "Re-entry" below).

If `VALUE` is `nil` (which is what a gate returns when it suppresses),
`publish!` is a no-op. That nil short-circuit is what makes the
pipeline form below read top-to-bottom.

`publish!` and `publish-to!` are now identical (`publish-to` predates
the merge, when `publish` was a strict-string-only sibling). Use
`publish!`.

## Re-entry

By default, a publish from inside a rule is delivered to subscribers
and the LVC, but is not matched against other rules. That keeps rule
graphs predictable: one inbound message produces one round of rule
evaluation, full stop.

Opt in per rule with `:reentrant true` between the filter and the body:

```edn
(on "devices.*" :reentrant true
  (json-demux! "temp" "hum" payload))

(on "devices.*.temp"
  (-> payload-float
      (round 0)
      (publish! (subject-append "stable"))))
```

The first rule is reentrant, so its demuxed `devices.kitchen.temp`
emission re-enters rule evaluation and the second rule fires on it.
The second rule is not reentrant, so its `...temp.stable` emission
goes to subscribers only and stops there.

Re-entry matches rules by subject, not by file position: the emitted
message can trigger matching rules before or after the emitting rule.
Rule evaluation still follows source order for a given subject, and
re-entry runs synchronously before the outer publish continues to later
matching rules.

A reentrant rule whose emission matches its own filter would loop
forever, so re-entry is depth-capped (default 8). The original PUB
runs at depth 0, and each level of re-entry increments by one;
emissions past the cap are dropped and logged to stderr.

`:reentrant` is the only rule option currently recognised. Unknown
options are a load error.

## Threading with `->`

`(-> X f1 f2 ...)` starts with `X`, calls `f1`, feeds that result into
`f2`, and so on. At each step, the current value is inserted as the
**last** argument of the form. A bare symbol `f` is treated as the call
`(f)`. Last-arg threading fits this dialect because every
stateful/transform op takes the value last:

The simplest case:

```edn
(-> x f1 f2)
```

means:

```edn
(f2 (f1 x))
```

> If you know Clojure: monoblok's `->` is deliberately **thread-last**.
> Clojure's `->` threads into the first argument, while Clojure's `->>`
> threads into the last. Patchbay only has `->`, and it behaves like
> Clojure `->>` because the DSL's operators are value-last.

```edn
(-> payload-float
    (round 1)
    (squelch)
    (publish! (subject-append "stable")))
```

expands to:

```edn
(publish! (subject-append "stable") (squelch (round 1 payload-float)))
```

The gates (`squelch`, `deadband`) return the value on pass and `nil`
on suppress, so `publish!` at the tail becomes a no-op when the
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
value through makes them compose directly with `->` and `publish!`.
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
      (publish! (subject-append "stable"))))

; Analog deadband: suppress changes below 0.5.
(on "sensors.*"
  (-> payload-float
      (deadband 0.5)
      (publish! (subject-append "delta"))))
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
        (publish! (subject-append "alert")))))
```

Stacks cleanly with value-based gates when you want both: "only if the
value changed, and at most once per 500ms."

```edn
(-> payload-float
    (round 1)
    (squelch)
    (hold-off 500)
    (publish! (subject-append "stable")))
```

`(throttle WINDOW MAX X)` is the related but distinct gate for "at most
MAX events per WINDOW." `hold-off` enforces a minimum interval between
passes; `throttle` enforces a maximum count over a sliding window (a
bucket of recent pass timestamps).
Reach for `hold-off` when you want to debounce a chattery edge, and
`throttle` when you want to cap a steady stream's rate without caring
about the spacing.

```edn
; Up to 5 alerts per minute, regardless of how they arrive.
(on "alerts.>"
  (-> payload
      (throttle :ms 60000 5)
      (publish! (subject-append "rate-limited"))))
```

`WINDOW` is either a bare integer `N` (tick window) or `:ms N` (time
window). Tick form counts the last `N` evaluations; time form counts
pass timestamps in the last `N` ms and is evicted by the server's
shared patchbay timer. Per (rule, subject, window-kind).

## Windowed aggregates

A small windowing family for smoothing and window-based triggers. State
is keyed by `(rule, subject, op, window kind)`, so a tick-windowed and a
time-windowed `moving-avg` on the same rule and subject keep independent
state. Repeating the same op/window kind in the same rule and subject
shares that slot; split it across rules if you need separate state.

A window is either a tick count or a wall-clock duration:

| form     | meaning                                |
|----------|----------------------------------------|
| `N`      | last N samples (fixed-cap ring)        |
| `:ms N`  | last N ms of wall-clock time           |

Rule of thumb: `!` marks forms that emit or otherwise have an effect,
`:ms N` marks wall-clock windows that may use the host clock, and bare
`N` marks tick/sample windows.

Wall-clock time is the ingress timestamp stamped once per inbound PUB
(same source `hold-off` reads). For windows that elapse without a new
PUB, each time-windowed slot exposes its next deadline and the server
keeps one rescheduled timer pointed at the earliest active deadline. On
each fire it scans due slots, evicts old samples, and closes any
time-windowed `bar!` whose window has fully passed, so a quiet feed
doesn't leave you with a stale aggregate or an unflushed bar.

On snapshot warm-start, existing time-window slots are re-armed when the
server starts. If a brief service bounce crossed a slot's deadline, that
slot fires immediately: old time-ring samples are evicted, and an
in-progress time bar closes with the last sample captured before the
bounce.

| form                       | returns | cost                                |
|----------------------------|---------|-------------------------------------|
| `(moving-avg WINDOW X)`    | number  | O(1) per update for tick form       |
|                            |         | O(n) per update for `:ms` form      |
| `(moving-sum WINDOW X)`    | number  | same as `moving-avg`                |
| `(moving-max WINDOW X)`    | number  | O(n) scan over the live window      |
|                            |         | O(n) per update for `:ms` form      |
| `(moving-min WINDOW X)`    | number  | same as `moving-max`                |

Tick rings allocate `N × 8 B` and `N` is fixed at first call. Time rings
grow with event rate × window: storage is unbounded in principle,
bounded in practice by however many samples arrive inside the window.
Aggregate readers such as `moving-min` and `moving-max` do an O(n) scan
over the live window, which is fine at sensor / ticker rates; if you
want sub-microsecond updates over wide time windows, prefer the tick
form with a generous `N`.

```edn
; Smooth with a 10-sample moving average, and only emit when the
; smoothed value drifts by at least 1.0.
(on "sensors.*"
  (-> payload-float
      (moving-avg 10)
      (deadband 1.0)
      (publish! (subject-append "smoothed"))))

; Same idea, but window over the last 5 seconds of wall-clock time
; instead of the last N samples. Useful when sample cadence varies and
; you want a stable "last 5 seconds" view.
(on "sensors.*"
  (-> payload-float
      (moving-avg :ms 5000)
      (publish! (subject-append "avg5s"))))

; Volatility detector: alert if the spread over the last 20 samples
; exceeds 5 units. Ring ops return numbers, so the non-threaded form
; is often clearer when you need multiple windows at once.
(on "sensors.*"
  (when (> (- (moving-max 20 payload-float)
              (moving-min 20 payload-float)) 5.0)
    (publish! (subject-append "volatile") payload)))
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

`rate` requires the `:ms N` form: "rate over the last N samples" has
no time unit, so passing a bare tick count is an argument error. `X` is
evaluated but its value ignored: the op counts pushes, not magnitudes.
Useful for "events/sec on this subject," "alerts/sec," etc.

`percentile`, `median`, `stddev`, and `variance` all read the window
and return immediately (no warm-up). On the first sample they return
that sample (or 0 for variance/stddev). Cost is O(n log n) for
percentile/median (sort copy) and O(n) for stddev/variance.

```edn
; Latency p99 over the last 200 samples. Useful in front of a SLO
; gate: only alert when sustained tail behaviour is bad, not a single
; outlier.
(on "rpc.latency.*"
  (-> payload-float
      (percentile 200 0.99)
      (publish! (subject-append "p99"))))

; Live event rate per subject, expressed in Hz.
(on "events.>"
  (-> payload
      (rate :ms 1000)
      (publish! (subject-append "rate-hz"))))

; Volatility flag: alert if the price stddev over the last minute
; jumps above 0.5.
(on "MARKET.*"
  (when (> (stddev :ms 60000 payload-float) 0.5)
    (publish! (subject-append "volatile") payload)))
```

### Clock-emitting forms

Most patchbay forms run only while handling an inbound PUB. These forms
store per-slot deadlines driven by the server's single rescheduled
patchbay timer, so they can publish after time passes even if the feed
goes quiet.

| form                                  | behavior                                      |
|---------------------------------------|-----------------------------------------------|
| `(on-silence :ms N BODY...)`          | evaluate BODY if no matching PUB arrives for N ms |
| `(dropout :ms N :lost LOST :found FOUND)` | silence runs LOST once; next match runs FOUND once |
| `(debounce! :ms N SUBJECT VALUE)`     | publish the latest SUBJECT/VALUE after N ms of quiet |
| `(sample! :ms N SUBJECT VALUE)`       | publish the latest SUBJECT/VALUE every N ms after the first match |
| `(aggregate! :ms N SUBJECT :METRIC X)` | publish a time-window metric on the clock deadline |

`on-silence` is a special form: BODY is stored unevaluated and runs
later with the last subject and payload in scope. It is useful for
liveness and stale-device detection. `dropout` adds the matching
recovery side: the first heartbeat arms quietly, silence runs `:lost`,
and only the next heartbeat after that runs `:found`.

```edn
(on "devices.*.heartbeat"
  (dropout :ms 30000
    :lost  (publish! (subject-append "stale") "true")
    :found (publish! (subject-append "stale") "false")))
```

`debounce!` is a trailing-edge emitter. It differs from `deadband`:
`deadband` suppresses small value movement, while `debounce!` waits for
quiet time and then publishes the latest value.

```edn
(on "knobs.*"
  (debounce! :ms 250
    (subject-append "settled")
    payload))
```

`sample!` keeps a regular cadence once the first matching message has
arrived. New messages update the retained value without moving the
cadence.

```edn
(on "sensors.*"
  (sample! :ms 1000
    (subject-append "sampled")
    payload))
```

`aggregate!` is the clock-emitting sibling of the value-returning
windowed aggregate forms. It supports `:avg`, `:sum`, `:min`, `:max`,
`:count`, and `:rate`, and publishes to the subject you provide.

```edn
(on "rpc.latency"
  (aggregate! :ms 10000 "rpc.latency.avg10s" :avg payload-float))
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
  (transition (> (moving-avg 60 payload-float) 28.0)
    (publish! (subject-append "alert") "hot")
    (publish! (subject-append "ok")    "cool")))
```

Compared to pairing two edge-gate rules, this evaluates the predicate
once, keeps one `moving-avg` ring instead of two, and doesn't renumber
when you later add or remove rules around it.

## JSON payloads

Most off-the-shelf sensors and gateways emit JSON frames (`{"temp":12.04,"hum":80}`),
not bare scalars. Two ops bridge that gap. Both accept top-level keys and
dotted object paths up to four levels deep (`"a.b.c.d"`), but not arrays
or full JSONPath. Escapes and
`\uXXXX` sequences are handled correctly.

| form                          | what it does                                                                |
|-------------------------------|-----------------------------------------------------------------------------|
| `(json-get KEY PAYLOAD)`      | return that field's value as a number, string, or boolean. Nil otherwise.   |
| `(json-demux! KEY ... PAYLOAD)` | publish each KEY/path value to `<subject>.<suffix>`. Side-effecting. Returns nil. |

`json-get` returns `nil` whenever the selected path can't be turned into
a scalar (key missing, payload not a JSON object, value is `null`, value
is an array or non-selected nested object). That nil flows through
`publish!` as a no-op, so a JSON-aware pipeline reads exactly like the
analog ones:

```edn
; Treat a JSON sensor frame like a scalar stream.
(on "sensors.*"
  (-> payload
      (json-get "temp")
      (round 1)
      (squelch)
      (publish! (subject-append "temp.stable"))))
```

`json-demux!` is the breakout: one wire carrying a multi-field frame
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
  (json-demux! "temp" "hum" payload))
```

For nested objects, use dotted paths with either JSON op. In
`json-demux!`, the full path is appended to the input subject by default:

```edn
; sensors.foo {"temp":{"c":20}} -> sensors.foo.temp.c 20
(on "sensors.*"
  (json-demux! "temp.c" payload))
```

`json-demux!` can flatten output subjects with `:leaf`, or override a
single output suffix with `[PATH SUFFIX]`:

```edn
; both emit sensors.foo.c for {"temp":{"c":20}}
(on "sensors.*"
  (json-demux! :leaf "temp.c" payload))

(on "sensors.*"
  (json-demux! ["temp.c" "c"] payload))
```

Both JSON ops support object paths up to four tokens deep, such as
`"a.b.c.d"`. Deeper paths are rejected when the rule runs. If a key is
missing, null, an array, or a non-selected nested object, `json-demux!`
skips it silently rather than emitting an error. Numbers come out
canonically formatted, strings are unquoted, booleans render as `true` /
`false`.

Both ops are arity-flexible at the **value-last** end (`PAYLOAD` is the
last argument), matching the rest of the dialect, so you can just as
easily write the threaded form `(-> payload (json-get "temp") ...)`.

## Bars

`(bar! WINDOW X)` is an OHLC bar accumulator for streams where you want
open / high / low / close summaries instead of every tick. `WINDOW` is
either `N` (close every N samples) or `:ms N` (close every N ms of
wall-clock time, aligned to `floor(now/N)*N`). Each call
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
boundary, or on the slot's exact deadline timer if no further samples
arrive.

```edn
; 60-tick OHLC bars per symbol on a market feed.
(on "MARKET.*"
  (bar! 60 payload-float))

; 1-minute aligned OHLC bars on the same feed. Bars cover wall-clock
; minutes (00:00..00:01, 00:01..00:02, ...) regardless of how many
; ticks land in each.
(on "MARKET.*"
  (bar! :ms 60000 payload-float))
```

A subscriber to `MARKET.AAPL.bar.>` then sees a clean burst of four
messages per closed bar, in `open / high / low / close` order, with no
intermediate per-tick noise.

`bar!` is fixed-size per stream, but the stream cardinality is yours to
budget. Each distinct subject seen by the bar rule gets a long-lived
state slot keyed by `(rule, subject, window-kind)`; closing a bar resets
the in-progress values but does not remove that slot. A raw trade feed
shaped like `T.<MARKET>.<SYM>` or a demuxed price stream shaped like
`T.<MARKET>.<SYM>.p` across thousands of symbols and tens of markets can
therefore retain tens or hundreds of thousands of bar slots. Narrow the
filter when you can, avoid unnecessary subject-token expansion, and
measure the shape you plan to run. The
[`json-massive` cardinality probe](../examples/json-massive/bar-cardinality.edn)
and
[`measure-bar-cardinality.sh`](../examples/json-massive/measure-bar-cardinality.sh)
give a repeatable RSS check for this case.

If another rule in the same patchbay should consume those derived bar
subjects, mark the bar-building rule `:reentrant true`. The emitted
subjects have two extra tokens (`MARKET.AAPL.bar.close`), so they do
not match the original `MARKET.*` filter and will not feed back into
the bar builder.

Volume isn't reported. For tick bars it's always exactly N; for time
bars, pair the rule with `(count!)` if you want the per-bar tick count.

## Running counters

`(count!)` is a side-effecting running counter, per `(rule, subject)`.
Each call increments and publishes the new total to `<subject>.count`.
Returns nil so it threads or sits in a `do` block without disturbing the
value being passed through.

With one optional argument, `(count! COND)` only increments when `COND`
is truthy (any value type, same rules as `if` / `when`). The predicate
itself is fully general, so it composes with everything the dialect
already has (`contains?`, comparisons, `changed?`, `rising-edge`, ...).

```edn
; Total tick count per symbol, live on MARKET.AAPL.count etc.
(on "MARKET.*"
  (count!))

; Error-event counter per service.
(on "events.>"
  (count! (contains? payload "ERROR")))

; Threshold breaches.
(on "sensors.>"
  (count! (> payload-float 100)))
```

State is a single number, snapshot-persisted, so a restart resumes from
the last seen total instead of zero. Subscribe to `<subject>.count` (or
`$LVC.<subject>.count` for the latest value) to watch a counter live.

## Debug printing

`(print! X)` and `(print! LABEL X)` are debug aids: they write a single
line to stderr and return `X` unchanged, so they sit cleanly inside a
threading chain without changing semantics.

```edn
; See the value at each stage of a pipeline.
(on "sensors.*"
  (-> payload-float
      (print! "raw")
      (round 1)
      (print! "rounded")
      (squelch)
      (publish! (subject-append "stable"))))
```

Output:

```
print! [sensors.kitchen] raw = 21.4378
print! [sensors.kitchen] rounded = 21.4
```

`print!` is not a publish (it doesn't bump `$STATS.rules.<i>.emitted`)
and is not gated by `--trace`. The loader counts every `print!` it
sees and the server logs a single warning at startup, so a left-in
`print!` is visible in the boot log:

```
warning: patchbay contains 2 print! call(s) across 1 rule(s); will log
payload data to stderr and add per-call overhead (debug aid, do not
leave in production)
```

Use it while you're debugging, take it out when you ship.
