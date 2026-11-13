# YAML and JSON patchbays

## YAML sugar and .edn-to-.yml ports

YAML patchbay files are a monoblok source-file convenience. For Tinyblok or
Patchbay Lite, keep using EDN unless that repo explicitly has a YAML loader.

Read [the YAML schema](patchbay-yaml-schema.md) first. It is bundled with the
skill and synced from Monoblok's canonical `docs/patchbay-yaml-schema.md` by
`scripts/site-docs/build.py`; edit that source when maintaining the schema.
When working in a checkout, prefer its canonical schema if it differs from
the bundled copy. The checkout's `docs/patchbay.md#yaml-sugar-files` and
`docs/patchbay-cheatsheet.md#edn--json--yaml` offer optional background.
The compact schema is:

- YAML is lowered directly to the patchbay AST. It is not general YAML:
  supported syntax is spaces for indentation, block maps/lists, flow arrays,
  flow option maps as direct call arguments, quoted or unquoted scalars,
  numbers, booleans, `null` / `nil` / `~`, and `#` comments outside quoted text
  and flow arrays. Do not use other flow maps, anchors, aliases, tags, or
  multiline strings.
- The root document is a map. Supported top-level keys are `on`, `every`, `lvc`,
  `export`, deprecated `bridge`, and `import`.
- `lvc` accepts one string-like value, one `env: "NAME"` value, or a list of
  those values.
- `export` is a config map with fields such as `servers`, `name`, `creds`,
  `user`, `password`, `token`, `tls`, `tls-ca`, `tls-cert`, `tls-key`,
  `tls-skip-verify`, `origin-header`, `replay-header`, `connect-timeout-ms`,
  `ping-interval-ms`, `max-reconnect`, `reconnect-wait-ms`, and `export`.
  `bridge` has the same shape but is deprecated.
- `import` is a config map with the connection fields above plus `subject` /
  `subjects`, `origin-header`, and `max-pending`. That flat shape is one core
  NATS import. The nested shape uses `core:` and/or `streams:`, each a list of
  entry maps with their own connection fields; `streams` entries add `stream`,
  `consumer`, and optional `catch-up` (default true) and take one subject
  filter each.
- In top-level config string positions, `env: "NAME"` lowers to `(env "NAME")`.
  Do not use it inside rule bodies.
- `on` is a list of rule maps. Each rule requires `sub`, may include
  `reentrant: true` and a `when: EXPR` guard, and must include exactly one body
  key: `pipeline` or `do`. `thread` and `form` remain compatibility spellings.
  `every` is the same with `ms: N` in place of `sub`. There is no `body`, `from`, `steps`, `test`, or `then`.
- `pipeline` is a block list and lowers to `->`: the start value first, then the
  steps, as in the example below.
- `when: EXPR` sits beside the body key and lowers to `(when EXPR BODY)`.
- `do` accepts one expression (`do: [publish!, "out", $payload]`) or a block
  list of bodies evaluated independently, in order (`(do BODY...)`). A flow
  array headed by an operator is one call; a block list is a sequence.
  Nested body maps accept `pipeline` or `do`, optionally with `when`.
- `thread` remains an alias for `pipeline`; `form` remains accepted for one
  direct expression. Never combine aliases or body keys in one body map.
- Flow arrays in expression positions are call forms, with the operator as the
  first element: `[publish!, [subject-append, "stable"]]`. Operators that are
  YAML indicators must be quoted: `[">", $payload-float, 80]`, `["*", a, b]`,
  `["->", ...]`. `[vector, 1, 2, 3]` is a literal vector.
- `$name` is a bound value: `$subject`, `$payload`, `$payload-float`,
  `$payload-int`, `$payload-bool`, `$replaying`, `$reply-to`. Write
  `$replaying` in YAML (it lowers to `replaying?`); `$replaying?` is still
  accepted by monoblok but older standard YAML parsers reject it inside flow
  arrays. Bare `payload` without the `$` is a load error.
- Prefer option maps as direct call arguments: `[moving-avg, {ms: 5000}]`
  or `[publish!, "out", {dp: 1}]` in a pipeline. Entries expand to keyword/value
  pairs in source order. Maps must be nonempty, with unique unquoted keys from
  the option words below. Values may be expressions, including nested calls
  and vectors, but cannot be maps. Use a space after each colon.
- Existing bare option words (`ms`, `dp`, `subject`, `lost`, `found`, `leaf`,
  `date`, `hour`, `minute`, `avg`, `sum`, `min`, `max`, `count`, `rate`,
  `reentrant`) still lower to keywords (`ms` becomes `:ms`). Keep standalone
  flags such as `[now, date]` bare; do not invent a value to put them in a map.
  Never write a leading colon (`:ms`) in YAML.
- Regex patterns (experimental `matches?`, `re-capture`, `re-replace`) are
  string literals written in single quotes: `'\d+'` (raw; `''` is a quote).
  Double quotes follow YAML escapes: `"\\d+"` works, `"\d+"` is a load error.
  EDN uses `#"\d+"` and JSON `"\\d+"`. Quote the head: `["matches?", '^err', $payload]`.
- Every other string in a rule body must be quoted (`"clean.temp"`, `"hot"`).
  An unquoted word that is not a number, boolean, null, `$binding`, or option
  is a load error.
- Files are valid standard YAML, so check them with a normal parser as well as
  `monoblok --validate`.

```yaml
lvc:
  - "temp.>"

on:
  - sub: "temp.*"
    when: [">", $payload-float, 20]
    pipeline:
      - $payload-float
      - [round, 1]
      - [squelch]
      - [publish!, [subject-append, "stable"], {dp: 1}]

every:
  - ms: 5000
    do: [publish!, "monoblok.heartbeat", "alive"]
```

JSON patchbays follow the same ideas: arrays are call forms, bound values are
strings with a leading `$` (`"$payload"`, `"$payload-float"`, `"$replaying?"`), and vector data
is `{"vector": [...]}`. A plain `"payload"` string in JSON is just text, not an
error, so always write `"$payload"`. Do not use `{"vec": [...]}`. Regex patterns
in JSON are JSON-escaped strings: `["re-capture", "t=(\\d+)"]`.

Validate YAML ports with `monoblok --validate file.yml`. When behavior matters,
compare the original and ported files with `--soundcheck` using the same input
rows.
