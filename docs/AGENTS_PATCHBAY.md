# Patchbay agent instructions

Paste the block below into an LLM coding agent's project instructions
whenever you want the model to author or edit patchbay rule files for
monoblok or Tinyblok/Patchbay Lite. It is self-contained: it does not
assume the model has read either repo.

## Install

For Codex and other agents that read `AGENTS.md`, append this file to
the project's agent instructions:

```sh
# project-scoped
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/docs/AGENTS_PATCHBAY.md >> ./AGENTS.md
```

Codex discovers repository-scoped `AGENTS.md` automatically when it
runs in that tree. For Claude Code, append to `./CLAUDE.md` (or
`~/.claude/CLAUDE.md`) instead, or reference `@docs/AGENTS_PATCHBAY.md`
inline in a prompt. For other tools, paste or reference this file in
the equivalent project instruction area: Cursor rules, Aider
conventions, ChatGPT project instructions, or another agent-specific
memory file. For one-off use, reference `docs/AGENTS_PATCHBAY.md` in
the prompt and ask the model to follow it while editing `.edn` or `.yml`
patchbay files.

Scrutinise every line of a generated patchbay before running it anywhere
that matters. A file that passes `--validate` is well-formed, not correct,
and a `--soundcheck` transcript only covers the rows that were fed in.

Agent guidance
---

You are writing rules for **monoblok patchbay** or **Tinyblok/Patchbay
Lite**, a small s-expression DSL that runs on top of a NATS-compatible
message router. Monoblok rule files live on disk (conventionally
`patchbay.edn`) and are loaded at daemon startup. Tinyblok embeds
`patchbay.edn` into firmware and parses it at boot. When asked for a
patchbay file, output valid patchbay EDN by default, with no prose outside
EDN comments (`;` to end of line). If the user explicitly asks for `.yml`,
`.yaml`, `.json`, or a port to one of those formats, output that format
instead. When asked for companion files such as a host-side pump driver, keep
those files separate and make the driver publish subjects that the patchbay
actually matches.

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
runs BODY on a fixed period instead of on a publish (see "Heartbeats" below).

In full monoblok there are also top-level config forms: `(lvc ["filter"
...])` opts matching subjects into `$LVC.*` last-value streams, and optional
`(export ...)` forwards local publishes to a remote NATS cluster. Deprecated
`(bridge ...)` is accepted as a compatibility alias. Optional
`(import ...)` subscribes to a remote NATS cluster (core subjects, JetStream
durable consumers, or both) and feeds matching messages into patchbay as
private ingress. Imported raw subjects are not visible to direct monoblok
subscribers unless a rule republishes them explicitly.

## Target runtime: monoblok vs Tinyblok

This prompt covers both monoblok and Tinyblok/Patchbay Lite. Before authoring
target-specific rules, determine the target runtime:

- If the repo, path, command, or prompt clearly says monoblok daemon, use full
  monoblok patchbay.
- If it clearly says Tinyblok, ESP32, firmware, microcontroller, or Patchbay
  Lite, use Patchbay Lite.
- If the target is ambiguous and the answer would differ, ask the user to
  confirm: monoblok daemon or Tinyblok/Patchbay Lite. If the requested rule is
  genuinely portable ordinary `(on ...)` patchbay with no target-specific
  features, you may proceed and keep it in the portable subset.

For portable rules, prefer ordinary `(on ...)` rules; scalar bound values like
`subject`, `payload`, `payload-float`, and `payload-int`; arithmetic,
comparison, string, subject, state, window, clock, edge, bar, and `publish!`
forms. This subset works for both runtimes unless a local build disables a form.

Alert the user before using or recommending full monoblok-only features for a
Tinyblok file. This includes top-level `(every ...)`, `(lvc ...)` and `(export ...)`,
`hysteresis`, `ewma`, `last`, the funnel pattern, monoblok's `reply!` (see
"Request-reply" below; Tinyblok's `reply!` is different),
`(import ...)` and JetStream ingress, `replaying?`, daemon flags such as
`--snapshot`, runtime patch/config
loading, dynamic graph edits, server-side subscriptions beyond Tinyblok's fixed
request subjects, inbound/outbound bridges, LVC/snapshot policy, and
fleet-management behavior. Tinyblok's vendored core can parse `lvc` and
`export`/deprecated `bridge`, but they are not wired into firmware policy.

JSON/event-document processing is also outside Patchbay Lite by default.
Tinyblok can compile `json-get` and `json-demux!` behind
`CONFIG_TINYBLOK_PATCHBAY_JSON`, but that option is disabled by default for
firmware size. Do not introduce JSON forms in a Tinyblok patchbay unless the
user explicitly enables that Kconfig option and accepts the size tradeoff.

## Patchbay Lite additions

Tinyblok embeds `patchbay.edn` into the firmware image and parses it once at
boot. It supports normal `(on ...)` rules plus these Tinyblok-specific forms:

- `(pump "subject" :from c_symbol :type TYPE :hz N)` registers a timed local
  source. `TYPE` is `u32`, `i32`, `f32`, `u64`, or `uptime-s`; the C source is a
  zero-argument read such as `uint32_t name(void)`, `int name(void)`,
  `float name(void)`, or `uint64_t name(void)`. Add user-owned implementations
  to `main/c/user.c` or another C file listed by `main/CMakeLists.txt`, then
  register the native symbol in `main/c/tinyblok_patchbay.c`.
- `(fn name :from c_symbol :type TYPE)` exposes a compiled zero-argument helper
  as a rule value. `(fn name :from c_symbol :input bytes :type bytes)` exposes a
  byte transformer that can be called from request handlers as `(name payload)`.
  Scalar `:input` functions are firmware-version dependent; verify the local
  Tinyblok native symbol table before using them.
- `(on-req "subject" BODY)` registers a fixed NATS request subject. Tinyblok
  subscribes to it after each broker connect, evaluates `BODY`, and sends
  replies to the requester's reply inbox.
- `(reply! VALUE)` sends a request reply from inside `on-req` (Tinyblok only;
  full monoblok uses `reply!` inside ordinary `(on ...)` rules, see
  "Request-reply"). In a pipeline,
  `(-> uptime-s (reply!))` replies with the threaded value. With no argument,
  `reply!` replies with the original request payload.

Tinyblok's `publish!` queues outbound records into the firmware TX ring for the
main loop to drain to NATS. Keep pump, request, and publish paths deterministic:
no per-tick heap allocation, blocking I/O, runtime parsing, or unbounded work.

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

## YAML sugar and .edn-to-.yml ports

YAML patchbay files are a monoblok source-file convenience. For Tinyblok or
Patchbay Lite, keep using EDN unless that repo explicitly has a YAML loader.

When working inside the monoblok repo, read `docs/patchbay-yaml-schema.md`
first; it is the canonical YAML shape reference. Use
`docs/patchbay.md#yaml-sugar-files` for the narrative overview and
`docs/patchbay-cheatsheet.md#edn--json--yaml` for the short EDN/JSON/YAML
comparison. The compact schema is:

- YAML is lowered directly to the patchbay AST. It is not general YAML:
  supported syntax is spaces for indentation, block maps/lists, flow arrays,
  quoted or unquoted scalars, numbers, booleans, `null` / `nil` / `~`, and `#`
  comments outside quoted text and flow arrays. Do not use flow maps, anchors,
  aliases, tags, or multiline strings.
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
  key: `thread`, `do`, or `form`. `every` is the same with `ms: N` in place of
  `sub`. There is no `body`, `from`, `steps`, `test`, or `then`.
- `thread` is a block list and lowers to `->`: the start value first, then the
  steps, as in the example below.
- `when: EXPR` sits beside the body key and lowers to `(when EXPR BODY)`.
- `do` is a list of bodies and lowers to `(do BODY...)`.
- `form` is one direct expression for cases such as `transition`, `count!`, or
  `bar!`.
- Flow arrays in expression positions are call forms, with the operator as the
  first element: `[publish!, [subject-append, "stable"]]`. Operators that are
  YAML indicators must be quoted: `[">", $payload-float, 80]`, `["*", a, b]`,
  `["->", ...]`. `[vector, 1, 2, 3]` is a literal vector.
- `$name` is a bound value: `$subject`, `$payload`, `$payload-float`,
  `$payload-int`, `$payload-bool`, `$replaying`, `$reply-to`. Write
  `$replaying` in YAML (it lowers to `replaying?`); `$replaying?` is still
  accepted by monoblok but older standard YAML parsers reject it inside flow
  arrays. Bare `payload` without the `$` is a load error.
- A bare word that is an option (`ms`, `dp`, `subject`, `lost`, `found`, `leaf`,
  `date`, `hour`, `minute`, `avg`, `sum`, `min`, `max`, `count`, `rate`,
  `reentrant`) lowers to the keyword (`:ms`). Never write the colon form.
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
    thread:
      - $payload-float
      - [round, 1]
      - [squelch]
      - [publish!, [subject-append, "stable"], dp, 1]

every:
  - ms: 5000
    form: [publish!, "monoblok.heartbeat", "alive"]
```

JSON patchbays follow the same ideas: arrays are call forms, bound values are
strings with a leading `$` (`"$payload"`, `"$payload-float"`, `"$replaying?"`), and vector data
is `{"vector": [...]}`. A plain `"payload"` string in JSON is just text, not an
error, so always write `"$payload"`. Do not use `{"vec": [...]}`.

Validate YAML ports with `monoblok --validate file.yml`. When behavior matters,
compare the original and ported files with `--soundcheck` using the same input
rows.

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
  (publish! (str-concat "group." (subject-token 1)) payload))

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
    (publish! (str-concat "clean.event." (subject-token 2)) payload)))

; Outlier gate: forward only values at or below the rolling p99.
(on "in.reading.*"
  (when (<= payload-float (percentile 100 0.99 payload-float))
    (publish! (str-concat "clean.reading." (subject-token 2)) payload)))

; Flood verdict: one "block" on the way up, one "clear" on the way down.
(on "in.*.*"
  (transition (> (rate :ms 5000 payload) 2)
    (publish! (str-concat "waf.flood." subject) "block")
    (publish! (str-concat "waf.flood." subject) "clear")))

; De-identify by allow-list: name the fields that may leave and drop the
; identifying subject token. Fields that are not named are never forwarded.
(on "ward.*.*"
  (-> payload (json-get "hr") (publish! (str-concat "clean." (subject-token 1) ".hr"))))
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

## Last values and the nil-safe idiom

`(last SUBJECT)` returns the latest payload published on a literal SUBJECT as a
string, or nil when there is none. It reads the LVC, so SUBJECT must match a
top-level `(lvc ...)` filter (without one it is always nil). Arithmetic and
comparison on nil are type errors, so use a thread, which stops on nil:

```edn
(lvc ["temp.>"])

(on "temp.inside"
  (-> (last "temp.outside")
      (- payload-float)
      (publish! "temp.difference" :dp 1)))
```

`last` returns a string: arithmetic coerces numeric strings, but `:dp N` formats only numbers, so `(reply! :dp 1 (last "x"))` fails at run time. Wrap it, e.g. `(round 3 (last "x"))`, or omit `:dp`.

Thread-last puts the `last` value in the final argument, so this publishes
`payload - outside`. Do not write `(- (last "temp.outside") payload-float)`
unguarded; if you need the non-threaded form, guard it with `(and (last S) ...)`.

## Request-reply (monoblok)

`reply-to` is the request's reply subject (nil when the message had none).
`(reply! VALUE)` and `(reply! :dp N VALUE)` publish VALUE to it. It is a silent
no-op when there is no reply subject or VALUE is nil, and it threads like
`publish!`. `reply-to` is nil for imported messages, clock ticks, `every`
bodies, and re-entered evaluations, and always nil under `--soundcheck`.

```edn
(lvc ["temp.>"])

; nats req q.temp "" answers with the latest kitchen temperature.
(on "q.temp"
  (reply! (last "temp.kitchen")))
```

## Heartbeats: `every`

`(every :ms N [:reentrant BOOL] BODY...)` is top level, a sibling of `on`, and
never appears inside a rule body. `N` is a literal integer from 1 to 1e12. The
first tick schedules now + N without firing, and a missed interval fires once,
never a burst. Inside the body `subject` and `payload` are empty strings and
`reply-to` is nil. Use `(count! :subject "x.count")` rather than bare
`(count!)` there, since the default subject would be empty.

```edn
(every :ms 5000
  (publish! "monoblok.heartbeat" "alive"))
```

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

## Export form (optional, zero or one at top level)

```edn
(export
  :servers ["tls://a.example:4222" "tls://b.example:4222"]
  :creds   "/etc/monoblok/ngs.creds"
  :tls     true
  :name    "monoblok-prod-1"
  :origin-header true
  :replay-header true
  :export  ["telemetry.>" "alerts.>"])
```

Deprecated `(bridge ...)` is still accepted as a compatibility alias.
Other keywords: `:user` / `:password`, `:token`, `:tls-ca`, `:tls-cert`
/ `:tls-key`, `:tls-skip-verify` (dev only), `:connect-timeout-ms`,
`:ping-interval-ms`, `:max-reconnect` (-1 unlimited), `:reconnect-wait-ms`.
`:origin-header true` adds `x-monoblok: <hostname>` to forwarded messages.
`:replay-header true` adds `x-monoblok-replay: true` and
`x-monoblok-assumed-ts: <unix-ms>` to bridged replay output.
`:export` is a vector of subject filters; matched publishes are
forwarded as-is. Nothing flows back. Both `:servers` and `:export`
also accept the legacy `(...)` list syntax, but `[...]` is the
recommended form. LVC follows the same recommendation:
`(lvc ["sensors.>" "alerts.>"])`, with legacy `(lvc "sensors.>")`
still accepted.

String-valued top-level config positions can use `(env "NAME")`, for example
`:servers (env "NATS_SERVERS")` or `:token (env "NATS_TOKEN")`. Env values are
read once at patchbay load time, must be non-empty, and are not comma-split.

## Import form (optional, zero or one at top level)

```edn
(import
  :servers ["tls://a.example:4222" "tls://b.example:4222"]
  :creds   "/etc/monoblok/ngs.creds"
  :name    "monoblok-import-prod-1"
  :origin-header true
  :subject ["raw.>" "replay.>"]
  :max-pending 4096)
```

Connection keywords match `(export ...)`. `:subject` is a string or vector of
remote subject filters to subscribe to. With `(import ...)` configured, local
socket clients may still subscribe but client `PUB` commands are rejected.
Imported messages run through `(on ...)` rules but are not routed to local
subscribers as raw publishes; only explicit rule outputs become visible.
`:origin-header true` ignores remote messages that carry monoblok's
`x-monoblok` header. `:max-pending` bounds the NATS-to-loop queue; default is
4096 messages. `:subjects` is accepted as an alias for `:subject`. Import
counters are published as `$STATS.import.received`, `.processed`, `.dropped`,
and `.failed`.

The flat shape above is one core NATS import. The nested shape scopes several
sources, each entry a vector with its own connection keywords:

```edn
(import
  :core    [[:servers ["nats://a.example:4222"] :subject ["raw.>"]]]
  :streams [[:servers ["nats://a.example:4222"]
             :subject  "sensors.>"
             :stream   "SENSORS"
             :consumer "monoblok-sensors"
             :catch-up true]])
```

`:streams` entries consume JetStream durable pull consumers. Each entry takes
one subject filter plus `:stream` and `:consumer` (both required) and optional
`:catch-up` (default true). monoblok is a JetStream consumer only, never a
JetStream server. With catch-up on, the stream is replayed up to its startup
high-water before the local listener opens, using each message's JetStream
timestamp as the clock, so `:ms` windows and time bars close on event time.
During that phase rules see `replaying?` as true; afterwards the same consumer
continues live with `replaying?` false. A rule that should not act on history
(alerts, notifications) guards itself with `(when (not replaying?) ...)`. A
message is acked only after its rules and downstream publishes succeed, so a
failing rule causes redelivery.

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
  (dropout :ms 30000
    :lost  (publish! (subject-append "stale") "true")
    :found (publish! (subject-append "stale") "false")))

(on "dials.*"
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

## Cheap validation

When you can run local commands, run both of these on every patchbay you
write or edit before handing it back. Do not report a patchbay as working on
the strength of reading it.

```sh
monoblok --validate patchbay.edn
printf 'sensors.temp|31\nsensors.temp|32\n' | monoblok --soundcheck --soundcheck-label --soundcheck-linger-ms 0 patchbay.edn
```

Use `./build/monoblok` inside the monoblok repo. If you cannot run commands,
say so and give the user these two lines to run themselves. Show the user the
soundcheck transcript, and state any threshold, window, or subject name you
assumed instead of taking it from their request, so they can review each line.

`--validate` form-lints the file and exits without opening a NATS
socket. It catches parse errors, invalid subjects, arity/type mistakes,
and rule bodies that fail against synthetic matching subjects.

For evaluator-level checks without starting a daemon, use
`--soundcheck`. It reads newline-delimited `SUBJECT|payload` rows on
stdin, echoes inputs to stdout, and prints any `publish!` emissions:

```sh
printf 'sensors.temp|31\n' | monoblok --soundcheck patchbay.edn
printf 'sensors.temp|31\n' | monoblok --soundcheck --soundcheck-label patchbay.edn
```

With `--soundcheck-label` every output row is prefixed `in|` or `out|`, so
an input row with no `out|` row after it produced nothing. Feed at least one
row per rule, on a concrete subject that the rule's filter matches, and
include rows that should be suppressed as well as rows that should pass.

Things that behave differently under `--soundcheck`:

- Clock and window state lingers after EOF (10 seconds by default) so
  delayed forms (`debounce!`, `on-silence`, `aggregate!`, time bars) and
  `every` bodies can fire. `--soundcheck-linger-ms 0` exits at EOF, so none
  of those emit; use it for rules with no clocked forms, and set the linger
  just above the longest `:ms` when you need to see a delayed emit.
- All rows arrive within the same few milliseconds, so `:ms` windows see one
  burst: `rate` reads high and `hold-off` / `throttle` suppress everything
  after their first passes. Check wall-clock behaviour against a running
  server.
- `:reentrant true` publishes are followed as on the server, and `last`
  reads a table that honours the file's `(lvc ...)` filters.
- `reply-to` is always nil, so `reply!` publishes nothing. Check request-reply
  against a running server with `nats req`.
- Negative payloads are fine in rows (`sensors.temp|-5`). With `nats pub`,
  pass them after `--`: `nats pub sensors.temp -- -5`.

For Tinyblok/Patchbay Lite, prefer the firmware repo's `make soundcheck` or
`make test` because they build the host validator against the vendored
monoblok patchbay and the embedded `patchbay.edn`.

## Companion pump drivers

This section is for host-side monoblok examples. For Tinyblok/Patchbay Lite,
do not create a Python pump driver unless explicitly asked; use top-level
`(pump ...)` declarations backed by compiled C symbols and register those
symbols in Tinyblok's native symbol table.

If you are asked to generate a pump driver for the topology, create a
small standalone script that publishes realistic sample traffic into
monoblok. Prefer Python with only standard-library imports. The driver
should:

- connect to `127.0.0.1:4222` by default, with `--host` and `--port`
- speak basic NATS over TCP: read `INFO`, send `CONNECT` and `PING`,
  wait for `PONG`, then send `PUB` frames
- publish exactly the input subjects the patchbay expects, not the
  derived output subjects
- emit JSON objects when the patchbay uses `json-demux!` or `json-get`
- include `--interval`, `--count`, `--seed`, and a useful `--verbose`
- run until interrupted when `--count 0`
- avoid external dependencies and avoid using a NATS client library

Use this skeleton as the starting point:

```python
#!/usr/bin/env python
"""Publish randomized telemetry into a monoblok/NATS endpoint."""

from __future__ import annotations

import argparse
import json
import random
import socket
import time


class NatsPublisher:
    def __init__(self, host: str, port: int, name: str) -> None:
        self.sock = socket.create_connection((host, port), timeout=5.0)
        self.file = self.sock.makefile("rb", buffering=0)
        info = self.file.readline()
        if not info.startswith(b"INFO "):
            raise RuntimeError(f"unexpected NATS greeting: {info!r}")

        connect = json.dumps(
            {
                "verbose": False,
                "pedantic": False,
                "name": name,
                "lang": "python",
                "version": "0",
            },
            separators=(",", ":"),
        )
        self.sock.sendall(f"CONNECT {connect}\r\nPING\r\n".encode("ascii"))
        self._read_pong()

    def publish(self, subject: str, payload: str) -> None:
        body = payload.encode("utf-8")
        self.sock.sendall(f"PUB {subject} {len(body)}\r\n".encode("ascii"))
        self.sock.sendall(body + b"\r\n")

    def close(self) -> None:
        self.sock.close()

    def _read_pong(self) -> None:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            line = self.file.readline()
            if line == b"PONG\r\n":
                return
            if line.startswith(b"-ERR"):
                raise RuntimeError(line.decode("utf-8", "replace").strip())
        raise TimeoutError("NATS server did not answer PING")


def publish_json(pub: NatsPublisher, subject: str, value: dict[str, object]) -> None:
    pub.publish(subject, json.dumps(value, separators=(",", ":")))


def run(args: argparse.Namespace) -> None:
    random.seed(args.seed)
    pub = NatsPublisher(args.host, args.port, "patchbay-pump")
    sent = 0

    try:
        while args.count == 0 or sent < args.count:
            publish_json(
                pub,
                "example.device-01.telemetry",
                {
                    "temperature": round(random.uniform(18.0, 35.0), 1),
                    "pressure": round(random.uniform(980.0, 1030.0), 1),
                    "mode": random.choice(["AUTO", "AUTO", "MANUAL", "FAULT"]),
                },
            )
            sent += 1

            if args.verbose:
                print(f"published {sent} messages")

            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        pub.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Pump sample telemetry into monoblok.")
    parser.add_argument("--host", default="127.0.0.1", help="NATS host")
    parser.add_argument("--port", type=int, default=4222, help="NATS port")
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--count", type=int, default=0, help="0 runs until interrupted")
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
```

## Authoring checklist

Before returning a patchbay file, verify:

1. For monoblok, every top-level form is `(on ...)`, `(every ...)`, `(lvc ...)`, or a single optional `(export ...)` / `(import ...)`. Deprecated `(bridge ...)` is accepted as an export alias. For Tinyblok/Patchbay Lite, top-level forms are ordinary `(on ...)` rules plus `(pump ...)`, `(fn ...)`, and `(on-req ...)`; do not rely on `(lvc ...)`, `(export ...)`, deprecated `(bridge ...)`, or `(import ...)` there without alerting the user that they are beyond Lite.
2. Every `publish!` target is a concrete subject (no `*` or `>`, no `$LVC.` or `$STATS.` prefix - those are read-only).
3. Every numeric op is fed a numeric value (`payload-float` / `payload-int` / arithmetic result), not raw `payload`.
4. `N` in `moving-*` is a literal integer and consistent per call site, subject, and window kind.
5. Pipelines built with `->` end in a side-effecting form (`publish!`); a pipeline that ends in a pure value is dead code.
6. Tinyblok `(pump ...)` and `(fn ...)` declarations name registered native symbols, and `on-req` handlers reply with `reply!` rather than assuming arbitrary runtime subscriptions.
7. Comments start with `;` and never leak prose outside them.
8. Every form you used is in this document. If the request needs something from "Forms that do not exist", say so instead of inventing a form.
9. You ran `monoblok --validate` and a `--soundcheck` with at least one row per rule (see "Cheap validation"), or told the user that you could not and gave them the commands.
