# monoblok JetStream replay design

## Status

Implemented v1 notes.

This document describes the JetStream ingress mode for `monoblok`.

The implemented v1 is deliberately simple:

- `monoblok` is a JetStream *consumer*, never a JetStream server
- replay is a startup phase, not a runtime mode
- the daemon catches up every configured stream before it serves anyone
- catch-up runs in the same process, on the main thread, with ports closed
- there is one atomic transition: virtual clock to real clock, ports closed to
  ports open
- there is no child process and no snapshot in the catch-up path

The goal is to let a `monoblok` process consume an existing JetStream stream,
rebuild correct patchbay-derived output over historical data as fast as the
machine allows, and then come up as an ordinary live JetStream consumer.

---

## Problem

`monoblok` patchbay currently treats `:ms` forms as processing-time forms.
Incoming messages are stamped with the daemon's current monotonic time and wall
clock, and clock-emitting forms are driven by real `uv_timer_t` deadlines.

That is correct for live core NATS traffic, but it is wrong for historical
JetStream replay when the desired output is "as of the original stream time."

Example:

- A JetStream stream contains last week's sensor data.
- `monoblok` should recompute minute bars, moving windows, rates, dropout
  signals, and `(now :hour)` outputs for last week.
- Replay should run as fast as the machine and downstream output path can
  handle.
- After catching up, the same process should continue as a live JetStream
  consumer.

`ReplayOriginal` is not enough for this. It preserves the original pacing, so a
week of history still takes a week to replay. `ReplayInstant` gives the right
throughput, but `monoblok` needs an event-time clock to make time-related
patchbay forms correct.

---

## Non-goals

This design intentionally avoids:

- implementing JetStream storage inside `monoblok`
- making `monoblok` durable for inbound messages — durability, acks,
  redelivery, and replication are JetStream's responsibility, upstream of
  `monoblok`
- changing the local NATS server to support JetStream clients
- a runtime catch-up/live mode change inside a daemon that is already serving
- catching up in a separate process and handing state over a snapshot
- treating reserved `Nats-*` headers as a public patchbay API
- mixing historical event-time replay and live socket publish time in the same
  state without an explicit clock policy
- hiding replay behavior behind implicit import behavior

Robustly accepting and retaining inbound messages is a NATS problem. Producers
that need durability publish into JetStream. `monoblok` reads from JetStream.
This keeps `monoblok` a conditioner, not a broker.

---

## Why a loading phase, not a live transition

An earlier draft of this design had the daemon catch up while already serving
subscribers, then perform a runtime handoff to live mode. That model is
fragile:

- While the main event loop drains a week of virtual deadlines as fast as it
  can, real subscribers and real timers are starved.
- Interleaving virtual-time catch-up work and processing-time live work puts two
  unrelated clocks into the same event loop and the same patchbay state.
- The handoff needs lag detection, a return-to-catch-up fallback, and careful
  ordering against newly arriving messages.

The fragility exists only because the daemon is trying to be live and catching
up at the same time.

The loading-phase model removes the conflict. Catch-up is part of starting up,
not a mode the daemon exits:

1. The daemon boots with listener ports closed and the patchbay clock in
   virtual mode.
2. It replays every configured JetStream import to that stream's startup
   high-water sequence, advancing the virtual clock from message metadata.
3. When all imports have reached high-water, it performs one atomic transition:
   switch the clock source from virtual to real, then open the listener ports.
4. From that point it is an ordinary live JetStream consumer. There is no
   special live mode and nothing to transition out of.

Because catch-up happens before anything is served, there is nothing to starve,
no second clock in flight, and no handoff. Inline catch-up on the main loop is
correct and sufficient.

Single process throughout means the patchbay state built during replay *is* the
live state — same `pb_program`, router, and LVC structures, same heap. The
transition does not serialize, write, or reload anything. It swaps a clock
source and opens sockets.

---

## Import configuration

JetStream ingress extends the existing top-level `(import ...)` shape with
explicitly scoped import kinds. Operators should be able to tell from the
patchbay config whether an import entry is a plain core NATS subscription or a
JetStream consumer.

Example shape:

```clojure
(import
  :core
  [[:servers ["nats://127.0.0.1:4222"]
    :subject ["raw.>"]]]

  :streams
  [[:servers ["nats://127.0.0.1:4222"]
    :subject ["sensors.>"]
    :stream "SENSORS"
    :consumer "monoblok-sensors"
    :catch-up true]])
```

YAML lowers to the same options:

```yaml
import:
  core:
    - servers: ["nats://127.0.0.1:4222"]
      subject: ["raw.>"]

  streams:
    - servers: ["nats://127.0.0.1:4222"]
      subject: ["sensors.>"]
      stream: "SENSORS"
      consumer: "monoblok-sensors"
      catch-up: true
```

Option meanings:

- `:core [...]` lists plain core NATS import entries. The older flat
  `(import :servers ... :subject ...)` shape remains a compatibility alias for
  one core entry.
- `:streams [...]` lists JetStream import entries.
- `:stream "NAME"` names the source stream. Required for JetStream import unless
  the implementation can unambiguously discover it.
- `:consumer "NAME"` names the durable consumer used by `monoblok`. It should be
  stable across restarts so a restarted daemon resumes from its acked position.
- `:catch-up true` records each stream's high-water sequence at startup and
  replays with the virtual event clock up to that sequence before the daemon
  opens its ports.
- `:catch-up false` skips historical replay. The loading phase is effectively
  empty and the daemon transitions to live almost immediately. If the named
  durable consumer already has a backlog, that backlog is consumed in live
  processing time, not replayed with the virtual clock.

The `:subject` option remains the filter subject list for each import entry. If
JetStream filter semantics cannot match a stream entry's configured list
exactly, validation should fail rather than silently widening the import.
The v1 runtime accepts exactly one subject filter per JetStream stream entry.
Operators who need multiple filters should configure multiple entries or use a
stream subject that already covers the desired set.

There is no `:max-catch-up-procs`, `:catch-up-mode`, or
`:inline-catch-up-max-ms` option. Catch-up is always inline in the loading
phase, so those controls have nothing to govern.

---

## Time model

JetStream ingress needs two clocks:

- **event time**: the timestamp used by patchbay evaluation
- **processing time**: the daemon's real clock, used for I/O, reconnects, and
  stats, and for patchbay timers after the transition to live

During the loading phase, patchbay evaluation uses event time from the
JetStream message. For ordinary JetStream consumer delivery, `nats.c` can read
this from `natsMsg_GetMetaData()`, which parses the ack reply subject metadata.
That timestamp is the JetStream server's stored message time.

If the domain requires producer event time rather than broker receive time, the
producer should carry it in the payload or an application-owned header. The
JetStream stored timestamp is still useful, but it is not the same thing.

Patchbay receives:

```text
now_ms  = event monotonic timeline in milliseconds
wall_ms = event UTC timestamp in milliseconds since epoch
```

`wall_ms` drives forms like `(now :date)`, `(now :hour)`, and `(now :minute)`.
`now_ms` drives windows, bars, rates, silence/dropout timers, debounce, sample,
and aggregate deadlines.

During the loading phase, `now_ms` is derived from `wall_ms` or from a
replay-local epoch offset. The key invariant is monotonicity per conditioning
domain. If a later message has an earlier event timestamp, the ingress path must
either reject it, clamp it to the current replay clock, or route it through a
separate out-of-order policy.

After the transition to live, future deadlines are scheduled against real
processing time using the offset between the last event time and the current
wall clock. The event clock remains based on JetStream message metadata so
behavior stays consistent under small transient consumer lag.

---

## Invariants

- Import/tap mode has exactly one write ingress into patchbay state.
- During the loading phase, listener ports are closed. No local client can
  connect, subscribe, or publish.
- After the transition, local core NATS clients may subscribe to derived
  output. Local client `PUB` frames are rejected while import/tap mode is
  active.
- Patchbay state, router state, and LVC state are owned by the main event loop
  for the entire life of the process, including the loading phase.
- The patchbay state populated during the loading phase is the same in-memory
  state used to serve live traffic. The transition does not copy or reload it.
- During the loading phase, patchbay timers are advanced by the virtual
  scheduler rather than by real sleeps.
- Historical replay never mixes event-time messages with processing-time local
  publishes in the same patchbay state. The loading phase guarantees this by
  construction: no local publishes are possible while ports are closed.

---

## Relationship to the existing importer

JetStream ingress is **not** the existing `mb_importer` with a flag set. The
two share intent — one write ingress into patchbay state — but the existing
importer's delivery model is wrong for replay in two specific ways, and the
JetStream ingress needs its own drain path.

The existing `mb_importer` is built for a live tap. `nats.c` delivers on its
own callback threads; each message is copied subject-and-payload into a bounded
ring under a mutex, and the loop thread is woken to drain it. That decoupling
is correct for a live firehose. For replay it has two problems:

- **The ring is lossy by design.** When the ring is full, the existing importer
  drops the message and increments a dropped counter. That is acceptable for a
  live tap of an unbounded stream. It is not acceptable for replay, where every
  message must be processed to rebuild correct patchbay state and where acks
  must follow evaluation. JetStream ingress needs *backpressure*, not drop: when
  the drain queue is full, stop fetching from JetStream (smaller pull batches,
  or pause the consumer) until the loop thread catches up. A dropped message
  during replay is silent state corruption.

- **The core import slot carries no metadata.** The existing slot holds only
  subject and payload bytes. JetStream replay needs the event timestamp and the
  stream sequence before evaluation — the timestamp to advance the virtual
  clock, the sequence to detect the high-water mark. The JetStream path reads
  that metadata from `natsMsg` before evaluation rather than copying messages
  through the core import ring.

Implications for the implementation:

- The JetStream ingress is a separate module from `importer.c`. It borrows the
  same ownership principle — `nats.c` work happens off-loop, patchbay evaluation
  happens on the loop thread — but keeps the handoff non-lossy and backpressured
  instead of using the core importer's bounded drop ring.
- During the loading phase the producer side is paced by JetStream pull
  batches, so backpressure is natural: the ingress does not request the next
  batch until the previous batch has been evaluated and acked.
- After the transition to live, each stream has a worker thread that fetches
  one message at a time and hands it to the libuv loop with `uv_async_t`. The
  worker waits for loop-thread evaluation before fetching the next message, so
  live JetStream ingress is backpressured rather than lossy.
- `pb_program_eval_publish_with_options()` and
  `pb_program_tick_with_options()` carry the `replaying` flag. The default
  wrappers keep existing core traffic behavior unchanged.

The existing core-NATS import path is untouched. This is an additional ingress,
selected by scoped `:streams` entries, not a modification of the existing one.

---

## Loading phase

At startup, for each configured JetStream import, the ingress records a stream
high-water sequence:

```text
catchup_to = stream.LastSeq
```

It then creates or resumes the durable consumer using `ReplayInstant`.

For each delivered message:

1. Read JetStream metadata.
2. Convert the message timestamp to event milliseconds.
3. Advance the patchbay virtual clock to that event time.
4. Drain any patchbay deadlines less than or equal to that event time without
   sleeping.
5. Evaluate the message with the event-time `now_ms` and `wall_ms`.
6. Ack only after the message has been accepted by the patchbay path.

When the delivered stream sequence reaches `catchup_to`, that import is marked
caught up.

Do not use `NumPending == 0` alone as the catch-up detector. Pending counts can
move while new messages arrive. A startup high-water sequence gives a clean,
fixed target.

The implementation records `stream.LastSeq` as the high-water target. Because a
filtered durable consumer may not receive the stream's last message when that
message is outside the filter, v1 also accepts `NumPending == 0` after a fetch
or timeout as "caught up for this filtered consumer." It never uses pending
zero by itself before the startup high-water target has been recorded.

If multiple JetStream imports are configured, v1 replays them serially in
configuration order and waits until *all* of them have reached their high-water
sequence. There is no per-stream readiness in v1 — readiness is a single,
whole-daemon property. Operators who need strict cross-stream event-time
ordering should put those subjects in the same stream, or wait for a later merge
policy; the first cut keeps stream ordering explicit and boring.

---

## The transition

When every configured import has reached its high-water sequence, the daemon
performs one atomic transition:

1. Drain any remaining due virtual timers across all patchbay programs.
2. Switch the patchbay clock source from virtual to real (`uv_now()` and the
   daemon wall clock).
3. Open the listener ports.
4. Start live JetStream workers.
5. Log that serial catch-up is complete.

After this point the daemon is an ordinary live JetStream consumer. The same
durable consumers keep delivering messages; only the clock source has changed.
There is no separate "live mode" to maintain and nothing to transition back
from. If a consumer falls behind after the transition, it falls behind in
processing time like any consumer — there is no return to virtual catch-up.

The transition performs no serialization. The patchbay, router, and LVC
structures built during the loading phase are carried straight into live
service.

---

## Readiness and visibility

During the loading phase the listener ports are closed, so `$STATS.*` subjects
cannot be used to report progress — nobody can subscribe yet. The implemented
v1 uses stderr log lines for visibility: stream high-water, filtered catch-up,
catch-up disabled, and the final "serial catch-up complete" transition line.

Once the ports are open, the existing `$STATS.import.*` subjects report
aggregate import state. Core imports and JetStream imports are summed into the
same counters: received, processed, dropped, and failed. JetStream acked and
redelivered counters are tracked internally but are not exposed as separate
per-stream stats subjects in v1.

A dashboard or operator should never have to infer progress from output volume.

---

## Output semantics

Replay output produced during the loading phase is generated output for the
historical event timeline. The question is what happens to it: a long replay
computes every intermediate bar, window, and derived value, and if those are
not published anywhere they are lost — only the final value of each subject
survives, in the LVC.

The wrong answer is "blast replay output onto the live subjects." If `monoblok`
is conditioning a subject that live consumers are watching, replaying a week of
history would deliver a week of derived messages in a burst as if they were
current. That is not running live; it is corrupting the live stream. Replay
output and live output must be distinguishable.

The right answer is that `monoblok` already has a language for "should this
message go out, and to what subject" — a patchbay rule. Output policy belongs
in the rules, not in a separate ingress config block.

### The `replaying?` symbol

The eval context carries a `replaying` flag. The ingress sets it true during
the loading phase and false after the transition. The patchbay exposes it to
rules as a symbol, the same way `payload` and `now` are symbols.

This is the only new capability needed for output policy. The ingress stays
dumb: it sets one boolean on the eval context. All policy is expressed in
rules:

```clojure
; suppress replay output entirely — live output only
(on "sensors.*" (when (not replaying?) (publish! "out.{1}" payload)))

; send replay output to a separate subject, live output to the real one
(on "sensors.*"
    (if replaying?
        (publish! "replay.out.{1}" payload)
        (publish! "out.{1}" payload)))

; no mention of replaying? — replay output flows exactly like live output
(on "sensors.*" (publish! "out.{1}" payload))
```

Because output routing is already a rule's job, a replay-prefixed namespace,
full suppression, or unrestricted flow are all just rules the operator writes.
There is no `:replay-output` config option and no destination enum.

### Default behavior

A rule that does not reference `replaying?` behaves **identically in both
phases**: replay output is published exactly as live output would be, just
computed against event time. Nothing is lost and nothing is special-cased. Only
rules that explicitly reference the symbol diverge between phases.

This makes the divergence opt-in and visible. It is still a footgun — a rule
gated on `replaying?` will behave differently after the transition, and an
operator could be surprised when output stops. `pb_soundcheck` / validation
should note when a program's output is replay-gated so the behavior is visible
rather than discovered at the transition.

### LVC

There is no separate "does LVC update during replay" decision. A value reaches
the LVC because a rule published it; whether a rule publishes during replay is
now controlled by `replaying?`. So LVC warming follows directly from the rules.
A program with no `replaying?` gates warms the LVC fully during the loading
phase, which is the usual desired behavior — a subscriber connecting right
after the transition sees a correct, warm cache.

### Exported (upstream) output

Output forwarded to an upstream NATS server via `(export ...)` follows the same
model: it happens because a rule published a subject the export config
forwards. An operator who does not want a week of history republished upstream
writes the upstream `publish!` behind `(when (not replaying?) ...)`, or routes
replay output to a subject the export config does not forward. No separate
"export during replay" flag is required.

### Note on headers

A header on replay output — for example an `Mb-Replay` application header so a
single subject can carry both replay and live messages and let consumers filter
— would be a natural extension. It is **not** in this design: the patchbay
`publish!` form does not currently accept headers, and adding header-carrying
publish is a separate change to the publish form and the router publish path.
If header-carrying `publish!` is added later, an `Mb-Replay` application header
would be the recommended tagging convention. Note this is an application-owned
`Mb-*` header and is distinct from the reserved `Nats-*` headers excluded by
the non-goals.

---


## Ack and failure policy

Acking before patchbay evaluation risks losing a message whose derived output
was not produced. Acking after evaluation preserves replay correctness.

The v1 implementation:

- acks after successful patchbay evaluation and accepted downstream publish
  attempts
- leaves idempotence to downstream subject design or a replay namespace
- tracks received, processed, acked, redelivered, and failed counters
- fails closed on malformed metadata; no processing-time fallback is currently
  implemented

Because durability and redelivery are owned by JetStream, a `monoblok` crash
during the loading phase is recoverable: on restart the durable consumer
resumes from its acked position and the loading phase runs again from there.

---

## Restart cost (future optimization)

In v1, a restart replays each stream from the durable consumer's acked position
to the current high-water sequence. For a consumer that is reasonably caught up,
that backlog is small and the loading phase is short.

If replaying from the acked position ever becomes too expensive at startup, a
periodic post-transition state snapshot could be added so a restart loads the
last snapshot and replays only from its recorded sequence forward. This is a
restart optimization for the daemon reloading its *own* prior state. It is
explicitly out of scope for v1, is independent of the loading-phase design, and
must not be confused with cross-process handoff — there is no second process.

---

## Implementation shape

The current implementation keeps JetStream separate from the core import path:

- `src/jetstream.c` owns JetStream connection, pull subscription, startup
  catch-up, live workers, metadata timestamp extraction, acking, and counters.
- `src/main.c` wires JetStream ingress before starting core imports. If any
  `:streams` entries exist, `mb_server_init()` binds but does not listen until
  JetStream catch-up has completed.
- `pb_program_tick_until()` drains due virtual deadlines up to each event-time
  message timestamp.
- `pb_program_eval_publish_with_options()` and
  `pb_program_tick_with_options()` carry `replaying=true` during catch-up and
  false during live delivery.
- `mb_server_patchbay_now_ms()` adds a one-time offset after the transition so
  ordinary timer scheduling uses the same patchbay clock surface.
- The loader stores imports as `pb_imports_config`, split into `cores[]` and
  `streams[]`. The older flat import shape still loads as one `cores[]` entry.
- `scripts/jetstream-smoke.sh` exercises a real JetStream server: historical
  replay through `replaying?`, live delivery after listener open, and source
  subject privacy.
- `examples/jetstream.yml` is the runnable YAML patchbay. `examples/jetstream.sh`
  starts a JetStream server, exports `JS_URL`, calls
  `examples/jetstream-populate.sh` to write `COUNT=1000` historical events by
  default, starts monoblok, and then publishes one live event.

The important boundary is that patchbay already has the right shape: evaluation
takes `now_ms` and `wall_ms`. JetStream ingress advances those values from
JetStream metadata during the loading phase, then performs one transition to
real time and open ports.

---

## Future refinements

These refinements are deliberately deferred. They do not change the v1
architecture; they add choices at the ingress edge, in front of the same
`now_ms` / `wall_ms` the patchbay already takes.

### Skip the loading phase when lag is trivial

v1 always runs the loading phase when `:catch-up true`. If the durable consumer
is already essentially caught up — a few messages behind, or seconds of event-
time lag — a virtual-clock loading phase is pure ceremony: those few messages
are as good as live, and replaying them with event time rather than processing
time makes no observable difference to any patchbay form.

v1.1 adds a pre-check that decides whether a loading phase is warranted at all:

```text
lag_messages = stream.LastSeq - consumer.AckFloor
lag_ms       = stream.LastTs  - consumer.LastActiveEventTime

if lag_messages <= live_start_max_messages AND lag_ms <= live_start_max_ms:
    boot straight to live   # ports open immediately, real clock, drain backlog live
else:
    run the loading phase   # ports closed, virtual clock, replay to high-water
```

The check is per import, but the daemon-level decision is "run the loading
phase if *any* import exceeds threshold." Caught-up imports then replay their
trivial backlogs essentially instantly inside the same loading phase, so no
import is special-cased.

This is distinct from `:catch-up false`. `:catch-up false` is the operator
saying "never replay, I do not want historical event-time output."  The lag
heuristic is the daemon saying "replay was requested, but there is so little to
replay that event time and processing time are indistinguishable, so skip the
ceremony." Thresholds of `0` must be allowed, meaning "always run the loading
phase if there is any backlog at all."

Proposed config:

```clojure
(import
  :streams
  [[:servers ["nats://127.0.0.1:4222"]
    :subject "sensors.>"
    :stream "SENSORS"
    :consumer "monoblok-sensors"
    :catch-up true
    :live-start-max-messages 1000
    :live-start-max-ms 5000]])
```

### Configurable event-time clock source

v1 uses the JetStream stored timestamp (`natsMsg_GetMetaData()`) as the only
event-time source. That is broker *receive* time — when NATS got the message,
not when the producer sampled it. It is free, always present, makes no payload
assumptions, and is close to monotonic by construction.

v1.1 makes the event-time source a first-class switch with three options:

- `:jetstream` — broker stored time. The v1 default.
- `:header` — an application-owned header carrying producer event time. Cheap
  to read, works for opaque/binary payloads.
- `:json` — a field extracted from a JSON payload by path. Closest to true
  producer intent, but requires the payload to be JSON and parseable, and means
  the ingress parses the payload before the patchbay sees it — a small new
  coupling worth stating explicitly.

Proposed config:

```clojure
:event-time {:source :jetstream}                       ; default

:event-time {:source :header :header "X-Event-Time" :format :rfc3339}

:event-time {:source :json   :path "meta.sampled_at"  :format :epoch-ms}
```

Whatever the source, the ingress resolves it to `now_ms` / `wall_ms`; the rest
of `monoblok` is unchanged. This is why it is an ingress-only concern and fits a
point release — provided v1 has already routed event time through the resolver
seam noted in *Implementation shape*.

Two sharp edges this introduces, both of which v1.1 must specify:

- **Missing or malformed timestamp.** A message with no header, unparseable
  JSON, or a path that does not resolve. v1.1 needs an explicit per-import
  policy: `:on-missing :fail` (reject/halt), `:on-missing :jetstream` (fall back
  to broker stored time), or `:on-missing :skip`. This is the concrete form of
  the "fail closed unless an explicit fallback is configured" rule already in
  *Ack and failure policy*.
- **Monotonicity becomes load-bearing.** Broker stored time is near-monotonic
  because NATS appends in receive order. Producer event time from a header or
  JSON field is not — late arrivals, cross-producer clock skew, and retries all
  break ordering. The monotonicity invariant in *Time model* is theoretical
  under the v1 default; under `:header` or `:json` it is real, and the
  out-of-order policy (reject / clamp / separate path) must become configurable
  alongside the clock source.

### Header-tagged replay output

v1 controls replay output entirely through the `replaying?` symbol (see *Output
semantics*): rules decide whether and where replay output is published. That
covers suppression and redirection to a separate subject, but not having replay
and live output coexist on the *same* subject with consumers filtering between
them.

v1.1 can add that by tagging replay output with an application header — the
convention being `Mb-Replay: true`. A live consumer filters those out; a
backfill job keeps only those.

This depends on a general patchbay capability that does not exist today:
**header-carrying `publish!`**. The current publish form takes only a subject
and payload. Adding headers touches the publish form signature, the EDN/YAML
rule syntax, the router publish path, and the export path — a cross-cutting
change that is useful well beyond replay (origin tagging, routing hints,
provenance). It therefore belongs as a general patchbay feature, specified
where general patchbay changes are specified, not inside this document.

Once header-carrying `publish!` exists, the cleaner design is for the JetStream
ingress to set `Mb-Replay` automatically while `replaying` is true, rather than
relying on every rule to add it. That pairs the tag with the existing
`replaying` flag and keeps it impossible to forget. `Mb-Replay` is an
application-owned `Mb-*` header and is distinct from the reserved `Nats-*`
headers excluded by the non-goals.
