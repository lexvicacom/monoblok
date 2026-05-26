# patchbay YAML schema

Patchbay YAML files are a small sugar layer for monoblok patchbay AST forms.
Files ending in `.yml` or `.yaml` are parsed as this subset, then lowered
directly into the same AST used by EDN and JSON patchbays.

This is not general YAML. The supported syntax is deliberately narrow:

- spaces for indentation; tabs are invalid
- block maps and block lists
- flow arrays, used for expression call forms: `[round, 1]`
- quoted or unquoted scalars
- numbers, booleans, `null` / `nil` / `~`
- comments with `#`, outside quoted text and flow arrays

Flow maps (`{...}`), anchors, aliases, tags, multiline strings, and other YAML
features are not part of the patchbay YAML subset.

## Root Document

The root document must be a map. Supported top-level keys are:

| key | type | lowers to |
|-----|------|-----------|
| `on` | list of rule maps | `(on ...)` forms |
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
    thread:
      - payload-float
      - [round, 1]
      - [publish!, "clean.temp", :dp, 1]
```

## Scalars

Scalar lowering depends on position.

In expression positions:

| YAML scalar | AST value |
|-------------|-----------|
| `payload` | bound symbol |
| `payload-float` | bound symbol |
| `payload-int` | bound symbol |
| `subject` | bound symbol |
| `replaying?` | bound symbol |
| `:ms` | keyword `:ms` |
| `:dp` | keyword `:dp` |
| `:subject` | keyword `:subject` |
| `true` / `false` | boolean |
| `null` / `nil` / `~` | nil |
| `123`, `12.5`, `-5` | number |
| other scalars | string |

In top-level config positions, string-like scalars are strings, not symbols.

Examples quote literal strings to keep them visually distinct from operators,
bound symbols, keywords, booleans, nulls, and numbers:

```yaml
export:
  servers:
    - "nats://127.0.0.1:4223"
  export:
    - "clean.>"
```

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
bodies can branch on `replaying?`. Multiple stream entries catch up serially in
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
    form:
      - do
      - [count!]
      - [publish!, "js.metrics.avg20", [round, 2, [moving-avg, 20, payload-float]]]
      - [bar!, :ms, 1000, payload-float]
      - [if, replaying?, [publish!, "js.replay.last-temp", payload], [publish!, "js.live.temp", payload]]

  - sub: "js.sensors.temp"
    when:
      test: [not, replaying?]
      then:
        thread:
          from: payload-float
          steps:
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

## `on` Rules

`on` is a list of rule maps. Each rule requires `sub` and exactly one body
shape.

```yaml
on:
  - sub: "raw.temp"
    thread:
      - payload-float
      - [round, 1]
      - [squelch]
      - [publish!, "clean.temp"]
```

Rule fields:

| field | type | meaning |
|-------|------|---------|
| `sub` | string | NATS subject filter matched by this rule |
| `reentrant` | bool | optional; feed this rule's emissions back into evaluation |
| `thread` | thread body | lowers to `(-> ...)` |
| `when` | when body | lowers to `(when TEST BODY)` |
| `do` | list of bodies | lowers to `(do BODY...)` |
| `form` | expression | direct expression body |
| `body` | expression | alias for `form` |

`sub` is always a string. It does not become a symbol.

## Thread Bodies

Thread bodies lower to the patchbay `->` form. The current value is threaded as
the last argument of each step.

Compact list form:

```yaml
thread:
  - payload-float
  - [round, 1]
  - [squelch]
  - [publish!, "clean.temp"]
```

Equivalent map form:

```yaml
thread:
  from: payload-float
  steps:
    - [round, 1]
    - [squelch]
    - [publish!, "clean.temp"]
```

Both lower to:

```edn
(-> payload-float
    (round 1)
    (squelch)
    (publish! "clean.temp"))
```

## When Bodies

`when` bodies require `test` and `then`.

```yaml
on:
  - sub: "sensors.temp"
    when:
      test: [>, payload-float, 80.0]
      then:
        thread:
          - payload
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

Use `do` when one rule should run multiple effects.

```yaml
on:
  - sub: "sensors.temp"
    do:
      - [publish!, "sensors.temp.raw", payload]
      - thread:
          - payload-float
          - [round, 1]
          - [publish!, "sensors.temp.clean"]
```

## Direct Forms

Use `form` or `body` when the rule body is already one expression.

```yaml
on:
  - sub: "logs.*"
    form: [when, [contains?, payload, "alert"], [publish!, "alerts.log", payload]]
```

`form` and `body` lower the supplied expression directly.

## Expression Arrays

Flow arrays are expression call forms. The first item is the operator symbol;
remaining items are arguments.

```yaml
[round, 1]
[moving-avg, :ms, 5000, payload-float]
[publish!, [subject-append, "stable"], :dp, 1]
[count!, :subject, "metrics.temp.count", [>, payload-float, 100]]
```

For `publish!`, `:dp N` formats a numeric payload with exactly `N`
decimal places, where `N` is an integer from 0 to 15.

These lower to:

```edn
(round 1)
(moving-avg :ms 5000 payload-float)
(publish! (subject-append "stable") :dp 1)
(count! :subject "metrics.temp.count" (> payload-float 100))
```

Nested arrays are nested calls. There is no YAML syntax for a literal vector
inside a rule body; use EDN or JSON when a body needs vector data.

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
    thread:
      from: payload-float
      steps:
        - [round, 1]
        - [squelch]
        - [publish!, "clean.temp"]
```

Validate YAML patchbays with the daemon before using them:

```sh
./build/monoblok --validate examples/import.yml
```
