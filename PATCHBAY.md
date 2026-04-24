# patchbay reference

The [README](./README.md) covers what patchbay is and what it's for.
This doc is the full reference for the DSL: syntax, bound symbols, and
every operator. For the 30-second pitch and worked examples, start
there; for "what does `(deadband ...)` actually do," this is the page.

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

## Windowed aggregates

A small ring-buffer family for smoothing and window-based triggers.
Each call site gets its own N-wide ring, keyed by `(rule, subject, op)`
, so `(moving-avg 10 ...)` and `(moving-max 10 ...)` in the same rule
maintain independent windows.

| form                 | returns | cost                                    |
|----------------------|---------|-----------------------------------------|
| `(moving-avg N X)`   | number  | O(1) per update (running sum)           |
| `(moving-sum N X)`   | number  | O(1)                                    |
| `(moving-max N X)`   | number  | O(1) amortized (monotonic deque)        |
| `(moving-min N X)`   | number  | O(1) amortized                          |

Memory is `N × 8 B` per `(rule, subject, op)` slot plus a small deque
for max/min. `N` is fixed at first call; don't change it between
invocations on the same rule.

```edn
; Smooth with a 10-sample moving average, and only emit when the
; smoothed value drifts by at least 1.0.
(on "sensors.*"
  (-> payload-float
      (moving-avg 10)
      (deadband 1.0)
      (publish-to (subject-append "smoothed"))))

; Volatility detector: alert if the spread over the last 20 samples
; exceeds 5 units. Ring ops return numbers, so the non-threaded form
; is often clearer when you need multiple windows at once.
(on "sensors.*"
  (when (> (- (moving-max 20 payload-float)
              (moving-min 20 payload-float)) 5.0)
    (publish (subject-append "volatile") payload)))
```

Ring ops return numbers, so they compose directly with the arithmetic,
comparison, and gating primitives above. No special pipeline syntax.

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
    (publish-to (subject-append "alert") "hot")
    (publish-to (subject-append "ok")    "cool")))
```

Compared to pairing two edge-gate rules, this evaluates the predicate
once, keeps one `moving-avg` ring instead of two, and doesn't renumber
when you later add or remove rules around it.
