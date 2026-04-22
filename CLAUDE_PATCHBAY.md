# Patchbay system prompt

Paste the block below into Claude's system prompt (or append to a project
CLAUDE.md) whenever you want the model to author or edit patchbay rule
files for monoblok. It is self-contained: it does not assume the model
has read the repo.

## Install

Append this file to a project CLAUDE.md so Claude Code picks it up
automatically when you are editing `.edn` rule files in that project:

```sh
# project-scoped (recommended)
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/CLAUDE_PATCHBAY.md >> ./CLAUDE.md

# or user-global
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/CLAUDE_PATCHBAY.md >> ~/.claude/CLAUDE.md
```

For one-off use, reference it inline in a prompt with
`@CLAUDE_PATCHBAY.md` (Claude Code inlines `@path` refs).

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
token, `>` matches one or more trailing tokens. Messages published
**from** a rule go out through normal fan-out but do **not** re-enter
rule evaluation (no loops).

There is also an optional `(bridge ...)` top-level form for forwarding
local publishes to a remote NATS cluster. It is export-only.

## Grammar (s-expressions)

`(head arg1 arg2 ...)`, no commas, no infix. Strings `"..."`, numbers
bare, `nil` / `true` / `false` are literals. Truthiness: only `nil` and
`false` are falsy (so `0` and `""` are truthy). Comments start with `;`.

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
- `(contains? h n)`, `(starts-with? h n)`, `(ends-with? h n)`
- `(subject-token N [S])` Nth dot token, 0-indexed, of `S` (default: `subject`)

Side effects:

- `(publish SUBJECT PAYLOAD)` validates and enqueues. Returns nil.
- `(publish-to SUBJECT VALUE)` same but args flipped for pipelines. No-op if VALUE is nil. Numbers are stringified canonically.

Idempotent filters (per-rule, per-subject state; first sight always passes / is treated as "no prior"):

- `(round N X)`, `(quantize STEP X)` pure
- `(squelch X)` pass iff X differs from last X, else nil
- `(deadband DELTA X)` pass iff numeric X moved by >= DELTA, else nil
- `(changed? X)` boolean predicate form of squelch
- `(delta X)` numeric difference since last X (0 on first sight)

Windowed aggregates (per `(rule, subject, op)` ring, N fixed at first call):

- `(moving-avg N X)`, `(moving-sum N X)`, `(moving-max N X)`, `(moving-min N X)`

Edge gates (take a boolean, fire once per transition; first sight never fires):

- `(rising-edge X)`, `(falling-edge X)`

## The `->` pipeline idiom

Because every transform / gate takes its value last, `->` reads
top-to-bottom as a data pipeline:

```edn
(-> payload-float
    (round 1)
    (squelch)
    (publish-to (subject-append "stable")))
```

Gates return `nil` on suppress; `publish-to` is a no-op on `nil`. That
is how a single pipeline "round, dedupe, emit" works without a `when`.

## Idioms to prefer

- For "dedupe after rounding / quantizing", thread `round` or `quantize` into `squelch` into `publish-to`. Don't hand-roll a last-value check.
- For analog noise, reach for `deadband` before `squelch`; for slow drift, wrap `moving-avg` in `deadband`.
- For alert + all-clear pairs on one predicate, use `transition`, not two `rising-edge` / `falling-edge` rules. It shares the ring and the prev slot.
- For one-sided alerts that still need to thread, use `rising-edge` / `falling-edge`.
- When you need multiple windows on the same stream (e.g. max - min spread), drop out of `->` and write the explicit nested form. `->` only threads one value.
- Keep subjects hierarchical: emit into `<input-subject>.<suffix>` via `subject-append` so downstream subscribers can pick the granularity they want.
- Don't publish back into a subject your own rule matches unless you have explicitly thought about it - rules don't loop, but a second rule on the output subject might.

## Anti-patterns

- Re-implementing `squelch` / `deadband` with `if` and a hand-rolled "last value" (there is no way to store state outside the gate primitives; the gates ARE the state).
- Using `=` to compare a number and a string. `=` is tag-strict; compare with `<` / `>` or parse first.
- Changing `N` in `(moving-avg N ...)` between invocations on the same rule. `N` is fixed on first call.
- Assuming `first sight` fires an edge. `rising-edge` / `falling-edge` / `transition` all stay quiet until they have seen at least one prior value.

## Bridge form (optional, zero or one at top level)

```edn
(bridge
  :servers ("tls://a.example:4222" "tls://b.example:4222")
  :creds   "/etc/monoblok/ngs.creds"
  :tls     true
  :name    "monoblok-prod-1"
  :export  ("telemetry.>" "alerts.>"))
```

Other keywords: `:user` / `:password`, `:token`, `:tls-ca`, `:tls-cert`
/ `:tls-key`, `:tls-skip-verify` (dev only), `:connect-timeout-ms`,
`:ping-interval-ms`, `:max-reconnect` (-1 unlimited), `:reconnect-wait-ms`.
`:export` is a list of subject filters; matched publishes are forwarded
as-is. Nothing flows back.

## Worked examples

```edn
; Fan high readings onto a dedicated sub-subject.
(on "sensors.*"
  (when (> payload-float 30.0)
    (publish (subject-append "high") payload)))

; Mirror anything that looks like an alert onto events.alerts.
(on ">"
  (when (contains? payload "alert")
    (publish "events.alerts" (str-concat subject ": " payload))))

; Clean up an RPM stream: 50rpm buckets, drop duplicates.
(on "car.*.rpm"
  (-> payload-float
      (quantize 50)
      (squelch)
      (publish-to (subject-append "stable"))))

; Smooth + deadband in one pipeline.
(on "sensors.*"
  (-> payload-float
      (moving-avg 10)
      (deadband 1.0)
      (publish-to (subject-append "smoothed"))))

; Sustained-heat alert and all-clear, one rule, one ring.
(on "temp.*.*"
  (transition (> (moving-avg 60 payload-float) 28.0)
    (publish-to (subject-append "alert") "hot")
    (publish-to (subject-append "ok")    "cool")))

; Volatility: two windows, non-threaded form.
(on "sensors.*"
  (when (> (- (moving-max 20 payload-float)
              (moving-min 20 payload-float)) 5.0)
    (publish (subject-append "volatile") payload)))
```

## Authoring checklist

Before returning a patchbay file, verify:

1. Every top-level form is `(on ...)` or a single optional `(bridge ...)`.
2. Every `publish` / `publish-to` target is a concrete subject (no `*` or `>`, no `$LVC.` or `$STATS.` prefix - those are read-only).
3. Every numeric op is fed a numeric value (`payload-float` / `payload-int` / arithmetic result), not raw `payload`.
4. `N` in `moving-*` is a literal integer and consistent per call site.
5. Pipelines built with `->` end in a side-effecting form (`publish-to` or `publish`); a pipeline that ends in a pure value is dead code.
6. Comments start with `;` and never leak prose outside them.
