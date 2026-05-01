# patchbay cheatsheet

For more details see [`patchbay.md`](./patchbay.md). If you're a coding assistant, see [`claude-patchbay.md`](./claude-patchbay.md).

## Top-level forms

| form | meaning |
|------|---------|
| `(on FILTER [:reentrant true] BODY)` | run BODY whenever an incoming subject matches FILTER. Wildcards: `*` one token, `>` tail. `:reentrant true` (optional, default false) feeds this rule's emissions back into rule evaluation; depth-capped at 8. |
| `(bridge :servers ... :export ...)` | optional, zero or one. Outbound NATS forwarder. See bridge keywords below. |

## Bound symbols

| symbol | value |
|--------|-------|
| `subject` | incoming subject (string) |
| `payload` | incoming payload (string of bytes) |
| `payload-float` | payload parsed as f64 (errors if not numeric) |
| `payload-int` | payload parsed as i64 (errors if not integer) |

## Special forms

| form | behaviour |
|------|-----------|
| `(if COND THEN ELSE?)` | branch on COND; ELSE optional, else nil |
| `(when COND BODY...)` | evaluate BODY iff COND truthy, return last; else nil |
| `(and X...)` / `(or X...)` | short-circuit, return last evaluated value |
| `(do X...)` | evaluate all, return last |
| `(-> X f1 f2 ...)` | thread X as the **last** arg of each form |
| `(transition BOOL RISING FALLING)` | fire RISING on false→true, FALLING on true→false; nil on first sight or no change |

## Side effects

Trailing `!` marks forms that emit (terminal effect, return nil). Scan a rule and the bangs are the lines that put bytes on the wire; everything else is pure. Un-banged spellings (`publish`, `publish-to`, `publish-to!`, `json-demux`, `count`, `bar`) still work as aliases for now. `publish!` and `publish-to!` are now identical (the args-flipped distinction is gone): both take `SUBJECT VALUE`, both coerce numbers, both no-op on nil VALUE.

| form | effect |
|------|--------|
| `(publish! SUBJECT VALUE)` | emit a publish; no-op if VALUE is nil; numbers coerced canonically |
| `(json-demux! KEY... PAYLOAD)` | break a top-level JSON object out onto `<subject>.<key>` for each KEY |
| `(count!)` / `(count! COND)` | running counter per (rule, subject); publishes to `<subject>.count` |
| `(bar! WINDOW X)` | OHLC bar; publishes `<subject>.bar.{open,high,low,close}` on each close |
| `(print! X)` / `(print! LABEL X)` | debug aid: writes one line to stderr, returns X unchanged so it threads. Not a publish; loader counts these and the server warns at startup. |

## Strings and subjects

| form | returns |
|------|---------|
| `(subject-append SUFFIX)` | `"<subject>.<suffix>"` |
| `(subject-with TOK ...)` / `(subject-with [TOK ...])` | tokens joined with `.`, publish-validated. Numbers/bools coerce; empty tokens error. |
| `(now :date)` / `(now :hour)` / `(now :minute)` | wall-clock UTC: `"YYYY-MM-DD"`, `"YYYY-MM-DDTHH"`, `"YYYY-MM-DDTHHMM"`. Subject-token-safe. Cached. `:minute` is high cardinality. |
| `(subject-token N)` / `(subject-token N S)` | Nth dot-token (0-indexed); nil if out of range |
| `(str-concat A B C ...)` | concatenated string |
| `(contains? COLL ITEM)` | bool. substring on strings, membership on vectors: `(contains? [1 2 3] payload-int)` |
| `(starts-with? TEXT PREFIX)` / `(ends-with? TEXT SUFFIX)` | bool (strings only) |

## Comparisons / arithmetic

| form | meaning |
|------|---------|
| `(= A B C ...)` | all equal (tag-strict) |
| `(< ...)` / `(<= ...)` / `(> ...)` / `(>= ...)` | pairwise numeric |
| `(+ X...)` / `(- X...)` / `(* X...)` / `(/ X...)` | arithmetic |
| `(not X)` | boolean negation |

## Numeric transforms

| form | meaning |
|------|---------|
| `(round DECIMALS X)` | X rounded to DECIMALS places |
| `(quantize STEP X)` | X snapped to nearest multiple of STEP |
| `(clamp LO HI X)` | X clipped to [LO, HI] |
| `(min A B...)` / `(max A B...)` | extrema |
| `(abs X)` | absolute value |
| `(sign X)` | -1 / 0 / 1 |

## Stateful gates

State is per `(rule, subject, op)` and survives snapshot reload. Gates
return their input on pass, `nil` on suppress; `publish!` is a no-op
on nil, so a chain of gates self-terminates.

| form | passes when |
|------|-------------|
| `(squelch X)` | X differs from last X seen (first sight passes) |
| `(deadband DELTA X)` | |X - last accepted X| ≥ DELTA |
| `(changed? X)` | bool form of `squelch`; true iff changed |
| `(delta X)` | numeric difference from last X (0 on first sight) |
| `(hold-off MS X)` | first sight, then any call ≥ MS ms after the prior pass |
| `(rising-edge X)` | X transitions falsy→truthy (first sight: nil) |
| `(falling-edge X)` | X transitions truthy→falsy (first sight: nil) |

## Windows

Windowed ops take a **window** as their first argument(s):

| form | meaning |
|------|---------|
| `N` (bare integer) | last N samples; fixed-cap ring |
| `:ms N` | last N ms of wall-clock time (ingress timestamp). The server walker also evicts on its ~500ms tick. |

Slots are keyed `(rule, op, kind, subject)`, so the same op with both
window kinds keeps distinct state.

## Windowed aggregates

| form | returns |
|------|---------|
| `(moving-avg N X)` / `(moving-avg :ms N X)` | running mean over the window |
| `(moving-sum N X)` / `(moving-sum :ms N X)` | running sum over the window |
| `(moving-max N X)` / `(moving-min N X)` (and `:ms` forms) | window extremes |
| `(rate :ms N X)` | events per second; **`:ms` only**. X is evaluated but ignored (counts pushes). |
| `(percentile N P X)` / `(percentile :ms N P X)` | Pth percentile (P in [0, 1]) |
| `(median N X)` / `(median :ms N X)` | sugar for `(percentile WINDOW 0.5 X)` |
| `(stddev N X)` / `(variance N X)` (and `:ms` forms) | population stats |

## Rate gates

Two flavours, picked by what you actually want:

| form | meaning |
|------|---------|
| `(hold-off MS X)` | min interval between passes (one timer, resets on each pass) |
| `(throttle WINDOW MAX X)` | max passes per window (sliding bucket of pass timestamps) |

## JSON

Top-level objects only (no JSON path, no array indexing, no nesting).
Backed by `std.json.Scanner`, so escapes / `\uXXXX` work.

| form | returns |
|------|---------|
| `(json-get KEY PAYLOAD)` | field as number / string / bool, or nil if missing / malformed / null / nested |
| `(json-demux! KEY... PAYLOAD)` | side-effecting; publishes each present field to `<subject>.<key>`, returns nil |

## Bridge keywords

A single optional `(bridge ...)` form at top level configures the
outbound NATS forwarder. Counters land on `$STATS.bridge.published` /
`.dropped` on the normal stats tick.

| keyword | type | meaning |
|---------|------|---------|
| `:servers` | vector of strings | server URLs (nats:// or tls://). e.g. `["nats://a:4222" "nats://b:4222"]` |
| `:name` | string | client name shown in remote monitoring |
| `:creds` | string | path to a .creds file (JWT+NKey) |
| `:user` / `:password` | strings | basic auth |
| `:token` | string | bearer token |
| `:tls` | bool | required for tls:// URLs |
| `:tls-ca` | string | CA cert path (PEM) |
| `:tls-cert` / `:tls-key` | strings | client cert + key (mTLS) |
| `:tls-skip-verify` | bool | dev only, insecure |
| `:connect-timeout-ms` / `:ping-interval-ms` | numbers | tuning |
| `:max-reconnect` | number | -1 for unlimited |
| `:reconnect-wait-ms` | number | tuning |
| `:export` | vector of strings | subject filters to forward (standard NATS wildcards). e.g. `["telemetry.>" "alerts.>"]` |
