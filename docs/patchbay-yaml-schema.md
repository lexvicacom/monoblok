# patchbay YAML schema

Patchbay YAML files are a small sugar layer for monoblok patchbay AST forms.
Files ending in `.yml` or `.yaml` are parsed as this subset, then lowered
directly into the same AST used by EDN and JSON patchbays. Every file is also
valid YAML for a standard parser.

The mental model, in four lines:

1. The first item of a flow array is the function: `[round, 1]`.
2. `$name` is a contextual value: `$payload-float`, `$subject`.
3. An option map supplies keyword/value pairs: `[moving-avg, {ms: 5000}, $payload-float]`.
4. Everything else is a quoted string: `[publish!, "clean.temp"]`.

This is not general YAML. The supported syntax is deliberately narrow:

- spaces for indentation; tabs are invalid
- block maps and block lists
- flow arrays, used for expression call forms: `[round, 1]`
- nonempty flow option maps as direct call arguments: `{dp: 1}`
- quoted or unquoted scalars (see the rules below)
- numbers, booleans, `null` / `nil` / `~`
- comments with `#`, outside quoted text and flow arrays

Other flow maps, anchors, aliases, tags, multiline strings, and other YAML
features are not part of the patchbay YAML subset.

## Root Document

The root document must be a map. Supported top-level keys are:

| key | type | lowers to |
|-----|------|-----------|
| `on` | list of rule maps | `(on ...)` forms |
| `every` | list of rule maps | `(every :ms N ...)` forms |
| `lvc` | string, env value, or list | `(lvc ...)` |
| `export` | config map | `(export ...)` |
| `bridge` | config map | deprecated `(bridge ...)` alias for export |
| `import` | config map | `(import ...)` |

Example:

```yaml
lvc:
  - "sensors.>"

export:
  servers: ["nats://127.0.0.1:4223"]
  export: ["clean.>"]

on:
  - sub: "sensors.temp"
    pipeline:
      - $payload-float
      - [round, 1]
      - [publish!, "clean.temp", {dp: 1}]
```

## Scalars

Scalar lowering depends on position. Top-level config positions (`lvc`,
`export`, `import`, and a rule's `sub`) take strings, quoted or not. Rule
bodies are stricter, so a typo cannot silently become text.

In rule bodies, an unquoted scalar must be exactly one of:

| unquoted scalar | AST value |
|-----------------|-----------|
| `123`, `12.5`, `-5` | number |
| `true` / `false` | boolean |
| `null` / `nil` / `~` | nil |
| `$subject`, `$payload`, `$payload-float`, `$payload-int`, `$payload-bool`, `$replaying`, `$reply-to` | bound symbol (`$replaying` maps to `replaying?`; otherwise `$` is dropped) |
| an option word such as `ms` or `dp` | keyword (`:ms`, `:dp`) |
| the first item of a flow array | operator name |

A quoted scalar (`"..."` or `'...'`) is always a string. That includes
`":ms"` (the text `:ms`) and `"$subject"` (the text `$subject`), which is how
you publish literal text that looks like a binding or option.

Single quotes are raw text, with `''` for one literal single quote. Double
quotes follow standard YAML escapes: `\\ \" \/ \n \r \t \0 \xHH \uHHHH`. Any
other escape, such as `\d`, is a load error that says to use single quotes.

Any other unquoted word in a rule body is a load error:

- `payload`, `replaying?` and other binding names without the sigil: the error
  suggests `$payload`.
- `$payload-flaot` and other unknown `$names`: the error names the unknown
  binding and lists the known ones. The list comes from the evaluator, so it
  always matches what the daemon supports.
- `:ms` and other colon keywords: the error names the replacement (`ms`).
- anything else, such as `alert` or a typo of an option word: the error says to
  quote it.

`$payload-bool` is true for `true`, `1`, `on` and false for `false`, `0`,
`off` (ASCII case-insensitive, no trimming); any other payload, including an
empty one, is a type error. `$payload-float` and `$payload-int` are also type
errors on an empty payload.

Examples quote literal strings to keep them visually distinct from operators,
bindings, options, booleans, nulls, and numbers:

```yaml
export:
  servers:
    - "nats://127.0.0.1:4223"
  export:
    - "clean.>"
```

For compatibility with older libyaml (including Ubuntu 22.04), use
`$replaying` in YAML; it lowers to the `replaying?` binding. The original
`$replaying?` spelling remains accepted by monoblok, but older standard YAML
parsers reject it inside flow arrays. Quoting a binding makes it literal text.

## Options

Prefer flow option maps as direct arguments of calls. Keys lower to keywords
and values lower to expressions, in source order. Maps must be nonempty and
keys must be unquoted, unique and drawn from the following option words:

`ms`, `dp`, `subject`, `lost`, `found`, `leaf`, `date`, `hour`, `minute`,
`avg`, `sum`, `min`, `max`, `count`, `rate`, `reentrant`

For example, `{ms: 5000}` lowers to `:ms 5000`. Values can include
bindings, nested calls, and vectors; nested maps and arbitrary map data are
not supported. Flow maps are not allowed at the root or in config positions.
Use a space after each colon for standard YAML compatibility. Bare option
words remain supported as compatibility syntax (`[moving-avg, ms, 5000]`),
and standalone keyword flags such as `[now, date]` stay bare. The set is
checked at load time: an unquoted word that is not an option, a binding, a
number, a boolean or a null is an error, so `[moving-avg, mz, 5000, $payload-float]`
fails instead of publishing the text `mz`.

```yaml
on:
  - sub: "sensors.*"
    do:
      - [publish!, "sensors.rounded", {dp: 1}, $payload-float]
      - [moving-avg, {ms: 5000}, $payload-float]
      - [count!, {subject: "metrics.temp.count"}]
      - [aggregate!, {ms: 1000}, "sensors.stats", avg, $payload-float]
      - [publish!, "clock", [now, date]]
      - [dropout, {ms: 500, lost: [publish!, "sensors.lost"], found: [publish!, "sensors.found"]}]
```

`subject` is both a binding name and an option word. Bare `subject` is the
option (`[count!, subject, "x"]`); write `$subject` for the value.

For `publish!`, `{dp: N}` formats a numeric payload with exactly `N` decimal
places, where `N` is an integer from 0 to 15.

## Environment Values

In top-level config string positions, a one-entry map of `env: "NAME"` lowers to
`(env "NAME")`. The environment variable is read once at patchbay load time.
The value must be present and non-empty, and is not split on commas.

```yaml
export:
  servers:
    env: "NATS_SERVERS"
  token:
    env: "NATS_TOKEN"
  export:
    - "sensors.>"
```

Environment values are for config fields only. They are not a rule-body form.

## `lvc`

`lvc` enables last-value-cache subjects. It accepts one string-like value or a
list of string-like values:

```yaml
lvc: "demo.>"
```

```yaml
lvc:
  - "demo.>"
  - "alerts.>"
```

```yaml
lvc:
  - env: "LVC_FILTER"
```

## `export`

`export` configures outbound forwarding to a real NATS cluster. It requires
`servers`. With no `export` filters, no subjects are forwarded.

```yaml
export:
  servers:
    - "nats://127.0.0.1:4223"
  name: "monoblok-export"
  origin-header: true
  replay-header: true
  export:
    - "sensors.*.stable"
    - "alerts.>"
```

Supported fields:

| field | type | meaning |
|-------|------|---------|
| `servers` | string or list of strings | NATS server URLs |
| `name` | string | remote client name |
| `creds` | string | path to a `.creds` file |
| `user` / `password` | strings | basic auth |
| `token` | string | bearer token |
| `tls` | bool | enable TLS |
| `tls-ca` | string | CA certificate path |
| `tls-cert` / `tls-key` | strings | client certificate and key |
| `tls-skip-verify` | bool | disable TLS verification; development only |
| `origin-header` | bool | add `x-monoblok` provenance header |
| `replay-header` | bool | add `x-monoblok-replay: true` and `x-monoblok-assumed-ts: <unix-ms>` to bridged replay output; live output omits both |
| `connect-timeout-ms` | number | connection timeout |
| `ping-interval-ms` | number | ping interval |
| `max-reconnect` | number | reconnect count; `-1` means unlimited |
| `reconnect-wait-ms` | number | reconnect wait |
| `export` | string or list of strings | subject filters to forward; optional, but usually present |

`bridge` has the same shape, but is deprecated:

```yaml
bridge:
  servers: ["nats://127.0.0.1:4223"]
  export: ["demo.>"]
```

## `import`

`import` configures inbound tap mode from a real NATS cluster. The flat shape
requires `servers` and `subject` or `subjects`.

```yaml
import:
  servers:
    - "nats://127.0.0.1:4223"
  name: "monoblok-import"
  subject:
    - "raw.>"
    - "replay.>"
  max-pending: 4096
```

The flat shape is a compatibility alias for one core NATS import. New configs
can scope core imports explicitly:

```yaml
import:
  core:
    - servers: ["nats://127.0.0.1:4223"]
      name: "monoblok-import"
      subject: ["raw.>"]
      max-pending: 4096
```

`streams` entries configure JetStream ingress and use the same connection
fields plus `stream`, `consumer`, and `catch-up`. In v1, each stream entry uses
one subject filter:

```yaml
import:
  streams:
    - servers: ["nats://127.0.0.1:4222"]
      subject: ["sensors.>"]
      stream: "SENSORS"
      consumer: "monoblok-sensors"
      catch-up: true
```

`catch-up: true` replays the durable consumer to the stream's startup high-water
sequence before monoblok opens its listener. During that loading phase, rule
bodies can branch on `$replaying`. Multiple stream entries catch up serially in
config order.

Small JetStream replay example:

```yaml
lvc:
  - "js.>"

import:
  streams:
    - servers:
        - env: "JS_URL"
      subject: "js.sensors.temp"
      stream: "SENSORS"
      consumer: "monoblok-jetstream-example"
      catch-up: true

export:
  servers:
    - env: "BRIDGE_URL"
  origin-header: true
  replay-header: true
  export:
    - "js.>"

on:
  - sub: "js.sensors.temp"
    do:
      - [count!]
      - [publish!, "js.metrics.avg20", [round, 2, [moving-avg, 20, $payload-float]]]
      - [bar!, {ms: 1000}, $payload-float]
      - [if, $replaying, [publish!, "js.replay.last-temp", $payload], [publish!, "js.live.temp", $payload]]

  - sub: "js.sensors.temp"
    when: [not, $replaying]
    pipeline:
      - $payload-float
      - [deadband, 0.25]
      - [publish!, "js.live.deadband"]
```

Connection fields match `export`: `servers`, `name`, `creds`, `user`,
`password`, `token`, `tls`, `tls-ca`, `tls-cert`, `tls-key`,
`tls-skip-verify`, `connect-timeout-ms`, `ping-interval-ms`,
`max-reconnect`, and `reconnect-wait-ms`.

Import-only fields:

| field | type | meaning |
|-------|------|---------|
| `subject` / `subjects` | string or list of strings | remote subject filters to subscribe to; JetStream stream entries accept one filter in v1 |
| `origin-header` | bool | ignore imported messages carrying `x-monoblok` |
| `max-pending` | number | bounded import queue length; default 4096 |
| `core` | list of maps | scoped core NATS import entries |
| `streams` | list of maps | scoped JetStream import entries |

Imported raw messages are private patchbay ingress. Direct monoblok subscribers
do not see them unless a rule republishes them.

## Operators That Need Quotes

The first item of a flow array is the operator. Operator names that start with
a letter are written bare: `[round, 1]`, `[publish!, "out"]`. Quote operators
containing `?`, such as `["contains?", $payload, "alert"]`, for older libyaml.
Operators made of punctuation must be quoted, because they are YAML indicator characters and a
standard YAML parser rejects or misreads them bare:

| write | not |
|-------|-----|
| `[">", a, b]` | `[>, a, b]` |
| `["<", a, b]` | `[<, a, b]` |
| `[">=", a, b]` | `[>=, a, b]` |
| `["<=", a, b]` | `[<=, a, b]` |
| `["=", a, b]` | `[=, a, b]` |
| `["*", a, b]` | `[*, a, b]` |
| `["+", a, b]` | `[+, a, b]` |
| `["-", a, b]` | `[-, a, b]` |
| `["/", a, b]` | `[/, a, b]` |
| `["->", a, b]` | `[->, a, b]` |

A bare punctuation operator is a load error that says to quote it, so a file
cannot depend on this parser being more lenient than real YAML. An unknown
operator name fails with `unknown form: NAME`.

```yaml
on:
  - sub: "sensors.temp"
    when: [">", $payload-float, 80.0]
    do: [publish!, "alerts.temp", ["*", $payload-float, 2]]
```

## `on` And `every` Rules

`on` is a list of rule maps. Each rule requires `sub`, optionally `when` and
`reentrant`, and exactly one body key. `every` is the same, with `ms: N`
(a number) in place of `sub`; it lowers to `(every :ms N ...)`.

```yaml
on:
  - sub: "raw.temp"
    pipeline:
      - $payload-float
      - [round, 1]
      - [squelch]
      - [publish!, "clean.temp"]

every:
  - ms: 1000
    reentrant: false
    do: [publish!, "tick", "alive"]
```

Rule fields:

| field | type | meaning |
|-------|------|---------|
| `sub` | string | `on` only: NATS subject filter matched by this rule |
| `ms` | number | `every` only: period in milliseconds |
| `reentrant` | bool | optional; feed this rule's emissions back into evaluation |
| `when` | expression | optional guard; lowers to `(when EXPR BODY)` |
| `pipeline` | list | body: lowers to `(-> ...)` |
| `do` | expression or list of bodies | body: one expression directly, or `(do BODY...)` for a sequence |
| `thread` | list | compatibility alias for `pipeline` |
| `form` | expression | compatibility spelling for one direct expression |

Exactly one body key is required: `pipeline` or `do`. The existing `thread`
and `form` spellings remain accepted for compatibility. Do not combine body
keys, including aliases, in one rule or nested body map. Any other key is a
load error. `sub` is always a string. It does not become a symbol.

## Pipeline Bodies

`pipeline` is a list. The first item is the start value and the rest are steps;
it lowers to the patchbay `->` form, where the current value is threaded as the
last argument of each step.

```yaml
on:
  - sub: "raw.temp"
    pipeline:
      - $payload-float
      - [round, 1]
      - [squelch]
      - [publish!, "clean.temp"]
```

This lowers to:

```edn
(-> payload-float
    (round 1)
    (squelch)
    (publish! "clean.temp"))
```

## When Guards

`when` sits beside the body key and guards it:

```yaml
on:
  - sub: "sensors.temp"
    when: [">", $payload-float, 80.0]
    pipeline:
      - $payload
      - [hold-off, 5000]
      - [publish!, "alerts.temp"]
```

This lowers to:

```edn
(on "sensors.temp"
  (when (> payload-float 80.0)
    (-> payload
        (hold-off 5000)
        (publish! "alerts.temp"))))
```

## Do Bodies

Use `do` for one expression or a block list of bodies evaluated in order.
Each body runs independently; results are not passed to the next body as they
are in a `pipeline`. Items are expressions or nested body maps (`pipeline` or
`do`, each optionally with its own `when`; `thread` and `form` also remain accepted).

```yaml
on:
  - sub: "sensors.temp"
    do: [publish!, "sensors.temp.raw", $payload]
```

A flow array headed by an operator is one call. A block list is a sequence of
bodies, even if its first item is a scalar expression. Existing flow sequences
of calls, such as `do: [[count!], [publish!, "out", $payload]]`, also remain
accepted. Empty sequences are rejected.

```yaml
on:
  - sub: "sensors.temp"
    do:
      - [publish!, "sensors.temp.raw", $payload]
      - when: [">", $payload-float, 30]
        pipeline:
          - $payload-float
          - [round, 1]
          - [publish!, "sensors.temp.clean"]
```

## Direct Forms

Use `do` when the rule body is already one expression. This also accepts scalar
expressions such as `do: $payload`. The compatibility spelling `form: EXPR`
continues to lower the expression directly.

When migrating a `form` call written as a block list (operator first), rewrite
it as a flow array under `do`. A block list under `do` always means separate
bodies, not the arguments of one call.

```yaml
on:
  - sub: "logs.*"
    do: [when, ["contains?", $payload, "alert"], [publish!, "alerts.log", $payload]]
```

## Vectors

A flow array headed by the reserved word `vector` is a vector value (data, not
a call):

```yaml
on:
  - sub: "sensors.*"
    do: [when, ["contains?", [vector, "temp", "hum", "batt"], [subject-token, 1]], [publish!, "seen", $subject]]
  - sub: "sensors.nums"
    do: [when, ["contains?", [vector, 1, 2, 3], $payload-int], [publish!, [subject-with, [vector, "n", $subject]], $payload]]
```

## Regex Patterns

> **Experimental.** `matches?`, `re-capture` and `re-replace` are new; the forms
> and pattern syntax may change between releases.

The pattern is the first argument and must be a string literal; the value being
tested comes last, so the forms thread in a `pipeline`. Write patterns in single
quotes (`'\d+'`); a double-quoted pattern needs doubled backslashes (`"\\d+"`),
and `"\d+"` is a load error. Quote the form head `matches?` because of the `?`.

```yaml
on:
  - sub: "in.>"
    pipeline: [$payload, ["matches?", '^err'], [publish!, "out.match"]]
  - sub: "cap.>"
    pipeline: [$payload, [re-capture, 't=(\d+)'], [publish!, "out.cap"]]
  - sub: "rep.>"
    pipeline: [$payload, [re-replace, '\s+', '_'], [publish!, "out.rep"]]
  - sub: "x.>"
    when: ["matches?", '(?i)<script|drop\s+table', $payload]
    do: [publish!, "out.x", $payload]
```

See [patchbay.md](patchbay.md#regular-expressions) for the pattern syntax.

## Expression Arrays

Flow arrays are expression call forms. The first item is the operator symbol;
remaining items are arguments. Nested arrays are nested calls.

```yaml
on:
  - sub: "sensors.*"
    do:
      - [publish!, [subject-append, "stable"], $payload-float, {dp: 1}]
      - [count!, {subject: "metrics.temp.count"}, [">", $payload-float, 100]]
      - [bar!, {ms: 1000}, $payload-float]
```

These lower to:

```edn
(publish! (subject-append "stable") payload-float :dp 1)
(count! :subject "metrics.temp.count" (> payload-float 100))
(bar! :ms 1000 payload-float)
```

## Import And Export Example

```yaml
import:
  servers:
    - "nats://127.0.0.1:14889"
  name: "monoblok-import-example"
  subject:
    - "raw.>"
  max-pending: 64

export:
  servers:
    - "nats://127.0.0.1:14889"
  name: "monoblok-export-example"
  origin-header: true
  replay-header: true
  export:
    - "clean.>"

on:
  - sub: "raw.temp"
    pipeline:
      - $payload-float
      - [round, 1]
      - [squelch]
      - [publish!, "clean.temp"]
```

Validate YAML patchbays with the daemon before using them, and check them with
a standard parser:

```sh
./build/monoblok --validate examples/import.yml
ruby -ryaml -e 'YAML.load_file(ARGV[0])' examples/import.yml
```

## JSON Differences

JSON patchbays follow the same rules where they apply. Bound values are
strings with a leading `$` (`"$payload"`, `"$payload-float"`, `"$replaying?"`);
an unknown `"$name"` is a load error. A plain `"payload"` is just a string.
For literal text that starts with `$`, write `{"str": "$text"}`. Config forms
(`lvc`, `export`, `import`) keep plain strings. Vector data is
`{"vector": [...]}`; the old `{"vec": [...]}` key is a load error.

## Migrating From The Old Shape

| old | new |
|-----|-----|
| `payload-float`, `payload`, `subject`, `replaying?`, `reply-to` | `$payload-float`, `$payload`, `$subject`, `$replaying`, `$reply-to` |
| `:ms`, `:dp`, `:subject` | `ms`, `dp`, `subject` |
| `[>, a, b]`, `[*, a, b]`, `[->, a, b]` | `[">", a, b]`, `["*", a, b]`, `["->", a, b]` |
| unquoted text such as `alert` | `"alert"` |
| `thread: {from: X, steps: [...]}` | `pipeline: [X, ...]` (start value first, then steps) |
| `when: {test: T, then: B}` | `when: T` beside one body key |
| `body: EXPR` | `do: EXPR` |
| `thread: [X, ...]` | `pipeline: [X, ...]` (`thread` remains accepted) |
| `form: EXPR` | `do: EXPR` (`form` remains accepted) |
| no vector literal | `[vector, 1, 2, 3]` |
| JSON `"payload"` as a bound value | JSON `"$payload"` |
| JSON `{"vec": [...]}` | JSON `{"vector": [...]}` |
