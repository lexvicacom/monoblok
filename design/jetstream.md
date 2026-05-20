# monoblok JetStream replay design

## Status

Draft / exploratory design notes.

This document describes a possible JetStream ingress mode for `monoblok`.

The goal is not to make `monoblok` a JetStream server. The goal is to let a
`monoblok` process consume an existing JetStream stream, rebuild correct
patchbay-derived output over historical data as fast as possible, and then stay
attached as a live consumer.

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
- changing the local NATS server to support JetStream clients
- treating reserved `Nats-*` headers as a public patchbay API
- mixing historical event-time replay and live socket publish time in the same
  state without an explicit clock policy
- hiding replay behavior behind implicit import behavior

This should be an explicit import mode, not just "import with an optional
timestamp."

---

## Import configuration

JetStream ingress should extend the existing top-level `(import ...)` shape with
explicit options. Operators should be able to tell from the patchbay config
whether import is a plain core NATS subscription or a JetStream consumer.

Example shape:

```clojure
(import
  :servers ["nats://127.0.0.1:4222"]
  :subject ["sensors.>"]
  :jetstream true
  :stream "SENSORS"
  :consumer "monoblok-sensors"
  :catch-up true
  :max-catch-up-procs 1)
```

YAML should lower to the same options:

```yaml
import:
  servers: ["nats://127.0.0.1:4222"]
  subject: ["sensors.>"]
  jetstream: true
  stream: "SENSORS"
  consumer: "monoblok-sensors"
  catch-up: true
  max-catch-up-procs: 1
```

Proposed option meanings:

- `:jetstream true` selects JetStream import instead of core NATS import.
- `:stream "NAME"` names the source stream. This should be required for
  JetStream import unless the implementation can unambiguously discover it.
- `:consumer "NAME"` names the durable consumer used by `monoblok`.
- `:catch-up true` records the stream high-water sequence at startup, consumes
  with virtual event time until that sequence is reached, then switches timer
  behavior to live mode.
- `:catch-up false` starts the consumer in live mode. It should not replay old
  data unless the named consumer's existing durable state already has a backlog.
- `:max-catch-up-procs N` limits how many child catch-up processes this
  `monoblok` parent may run at once.

The current `:subject` option remains useful as the filter subject list for the
JetStream consumer. If JetStream filter semantics cannot match the configured
list exactly, validation should fail rather than silently widening the import.

The consumer name should be stable across restarts for normal catch-up/live
operation. Ephemeral consumers are useful for ad hoc replay, but a durable name
is the safer default for a daemon expected to resume after a crash.

`max-catch-up-procs` is process-level coordination expressed in the import
configuration because imports create the catch-up work. If multiple import forms
eventually become legal, the value should be consistent across them or rejected
as ambiguous. For the current single-import shape, it simply caps child replay
parallelism for that daemon.

---

## Time model

JetStream ingress needs two clocks:

- **event time**: the timestamp used by patchbay evaluation
- **processing time**: the daemon's real clock, used for I/O, reconnects,
  stats, and live timers after catch-up

During historical catch-up, patchbay evaluation should use event time from the
JetStream message. For ordinary JetStream consumer delivery, `nats.c` can read
this from `natsMsg_GetMetaData()`, which parses the ack reply subject metadata.
That timestamp is the JetStream server's stored message time.

If the domain requires producer event time rather than broker receive time, the
producer should carry it in the payload or an application-owned header. The
JetStream stored timestamp is still useful, but it is not the same thing.

Patchbay should receive:

```text
now_ms  = event monotonic timeline in milliseconds
wall_ms = event UTC timestamp in milliseconds since epoch
```

`wall_ms` drives forms like `(now :date)`, `(now :hour)`, and `(now :minute)`.
`now_ms` drives windows, bars, rates, silence/dropout timers, debounce, sample,
and aggregate deadlines.

For a JetStream replay clock, `now_ms` can be derived from `wall_ms` or from a
replay-local epoch offset. The key invariant is monotonicity per conditioning
domain. If a later message has an earlier event timestamp, the ingress path must
either reject it, clamp it to the current replay clock, or route it through a
separate out-of-order policy.

---

## Invariants

- Import/tap mode has exactly one write ingress into patchbay state.
- Local core NATS clients may subscribe while import/tap mode is active.
- Local core NATS client `PUB` frames are rejected while import/tap mode is
  active, including JetStream catch-up/live mode.
- JetStream catch-up and live delivery use the same import-owned write path.
- Patchbay state, router state, and LVC state remain owned by the main event
  loop.
- Historical replay must not mix event-time messages with processing-time local
  publishes in the same patchbay state.
- During catch-up, patchbay timers are advanced by the virtual scheduler rather
  than by real sleeps.

This keeps the event-time model tractable. The daemon can serve subscribers
while replaying, but it does not accept a second producer clock into the same
conditioning state.

---

## Catch-up then live

At startup, the JetStream ingress records a stream high-water sequence:

```text
catchup_to = stream.LastSeq
```

It then creates or resumes a durable consumer using `ReplayInstant`.

For each message:

1. Read JetStream metadata.
2. Convert the message timestamp to event milliseconds.
3. Advance the patchbay virtual clock to that event time.
4. Drain any patchbay deadlines less than or equal to that event time without
   sleeping.
5. Evaluate the message with the event-time `now_ms` and `wall_ms`.
6. Ack only after the message has been accepted by the patchbay path.

When the delivered stream sequence reaches `catchup_to`, the daemon marks the
ingress as live.

Do not use `NumPending == 0` alone as the catch-up detector. Pending counts can
move while new messages arrive. A startup high-water sequence gives a cleaner
handoff point.

Catch-up may run in the parent process or in a short-lived child process. The
input continuity rule is the same either way: only one process owns the durable
consumer at a time.

---

## Virtual scheduler

During catch-up, patchbay deadlines must be drained by computation, not by real
sleep.

For a message at event time `T`:

```text
while next_patchbay_deadline <= T:
    pb_program_tick(program, router, next_patchbay_deadline, corresponding_wall_ms)

pb_program_eval_publish(program, router, subject, payload, T, event_wall_ms)
```

This lets forms like `bar! :ms 60000`, `aggregate!`, `sample!`, `debounce!`,
`dropout`, and `on-silence` observe the historical clock correctly while replay
runs as fast as possible.

After the ingress becomes live, the next deadlines should be scheduled against
real processing time using the offset between current event time and current
wall clock. If the last event time is close to real time, this should converge
to normal timer behavior.

If the consumer falls behind again after becoming live, the ingress can return
to virtual catch-up mode until the event clock catches real time again.

---

## Separate catch-up process

For small backlogs, the parent process can catch up in place. For large
backlogs, replay may consume CPU, allocate wide time windows, and produce a
large historical output burst. In that case the parent can spawn a short-lived
catch-up child.

High-level shape:

1. Parent starts with JetStream live import disabled.
2. Parent decides whether catch-up is small enough to run inline.
3. If not, parent starts a child `monoblok` process with the same patchbay
   config and an explicit catch-up mode.
4. Child binds the configured durable consumer name.
5. Child records `catchup_to = stream.LastSeq`.
6. Child consumes with `ReplayInstant`, evaluates with event time, and drains
   virtual patchbay deadlines without sleeping.
7. Child emits exported NATS subjects only if the catch-up output policy allows
   historical output to leave the process.
8. Child acks after successful evaluation/output acceptance.
9. After acking through `catchup_to`, child flushes due virtual timers, writes a
   snapshot, reports success to the parent, and exits.
10. Parent loads the snapshot, attaches the same durable consumer, and continues
    live.

The child should not run concurrently with the parent on the same durable
consumer. Sharing a durable would make ordering and state ownership harder to
reason about. The simple model is exclusive ownership: child owns the durable
during catch-up, then parent owns it after handoff.

Durable consumer continuity preserves input position. Snapshot continuity
preserves `monoblok` conditioning state. Handoff requires both.

---

## Catch-up placement heuristic

The decision to catch up inline or spawn a child should be based on estimated
catch-up duration and output cost, not raw pending count alone.

Useful inputs:

- message lag: stream high-water sequence minus the consumer ack floor
- time lag: stream latest timestamp minus the consumer's last acked event time
- measured replay/eval throughput for this patchbay on this machine
- output amplification: derived publishes per imported message
- whether catch-up output is allowed to reach live subscribers/export subjects
- estimated snapshot cost for the resulting state

Example policy:

```text
estimated_catchup_s = lag_messages / measured_replay_msgs_per_sec

if estimated_catchup_s <= inline_catchup_max_s:
    catch up in the parent process
else:
    spawn a catch-up child process
```

Configuration should make this explicit:

```clojure
(import
  :servers ["nats://127.0.0.1:4222"]
  :subject ["sensors.>"]
  :jetstream true
  :stream "SENSORS"
  :consumer "monoblok-sensors"
  :catch-up true
  :catch-up-mode :auto
  :inline-catch-up-max-ms 5000
  :max-catch-up-procs 1)
```

Proposed modes:

- `:catch-up-mode :inline` always catches up in the parent process.
- `:catch-up-mode :child` always uses a child process.
- `:catch-up-mode :auto` estimates catch-up cost and logs the decision.

If the estimate is missing or clearly unreliable, the conservative default is to
spawn the child or require an explicit operator choice. A bad inline estimate
can starve subscribers and timers in the parent.

If multiple JetStream imports need catch-up at once, the parent should be able
to stagger them. Running every backlog at full speed can overload the upstream
NATS server, the host CPU, disk, or downstream export targets.

Useful throttles:

- maximum concurrent catch-up imports
- maximum concurrent child catch-up processes, configured with
  `:max-catch-up-procs`
- per-import replay batch size
- per-import yield interval back to the parent event loop
- optional delay between starting catch-up jobs
- output/export rate limits for historical replay

An `:auto` implementation should prefer predictable progress over peak replay
speed. If several streams are behind, catching up one or two at a time is easier
to observe and recover than letting every stream compete at once.

---

## Catch-up visibility

Catch-up state should be visible through system subjects. Operators and local
subscribers need to know whether a stream is historical, live, stalled, or
handing off.

Proposed live status subjects:

```text
$STATS.import.<name>.mode              core | js-inline | js-child
$STATS.import.<name>.phase             live | catchup | handoff | stalled | failed
$STATS.import.<name>.stream            SENSORS
$STATS.import.<name>.consumer          monoblok-sensors
$STATS.import.<name>.catchup.to        <stream sequence>
$STATS.import.<name>.stream.seq        <last processed stream sequence>
$STATS.import.<name>.lag.messages      <count>
$STATS.import.<name>.lag.ms            <event-time lag>
$STATS.import.<name>.processed         <count>
$STATS.import.<name>.acked             <count>
$STATS.import.<name>.redelivered       <count>
$STATS.import.<name>.failed            <count>
```

The exact subject names can change, but the state must be explicit. A dashboard
or operator should not have to infer catch-up from output volume.

`<name>` should come from the import config's `:name` when present, otherwise a
stable sanitized form of the stream or consumer name. If the status subjects are
made LVC-enabled, late subscribers can immediately see that an import is in
catch-up mode.

Status should also be emitted during child-process catch-up. The parent can
publish child status by reading child progress messages, or the child can emit
status directly if it is allowed to publish system subjects. In either case,
there should be one authoritative visible status for each configured import.

---

## Snapshot handoff

When a child process performs catch-up, the parent must load the child's
snapshot before attaching the durable consumer for live delivery.

The child reports a small handoff record:

```text
caught-up
snapshot=/var/lib/monoblok/replay-123.snapshot
stream=SENSORS
consumer=monoblok-sensors
acked_seq=123456789
event_wall_ms=...
patchbay_config_hash=...
```

Parent handoff sequence:

1. Keep the parent JetStream consumer disabled.
2. Verify the child exited successfully.
3. Verify the snapshot patchbay/config hash matches the parent's loaded config.
4. Load snapshot state into the parent's own `pb_program`, router, and LVC
   structures on the main loop.
5. Restore the replay event clock from the snapshot metadata.
6. Attach the same durable consumer name.
7. Drain messages published after the child's acked sequence.
8. Enable live output behavior according to configuration.

The snapshot should carry enough metadata to make the handoff auditable:

- LVC entries
- patchbay state entries
- time-window samples
- bar state
- armed clock deadlines
- last event `now_ms` and `wall_ms`
- stream name, consumer name, and acked stream sequence
- patchbay/config identity

The parent should not import process memory or share runtime structs with the
child. The snapshot is the ownership boundary.

---

## Live handoff

The live handoff is not a change in source. The same durable consumer continues
delivering messages. What changes is timer behavior:

- before catch-up: advance and tick the patchbay clock without sleeping
- after catch-up: use real timers for future deadlines

The event clock should still be based on JetStream message metadata after
catch-up. That keeps behavior consistent if the consumer has small transient
lag. Live local wall clock should not replace JetStream event time in the same
state unless the operator explicitly chooses processing-time mode.

If event time lags real time by more than a configured threshold, expose that as
ingress lag. The daemon may keep operating, but dashboards and alerts should
show that patchbay outputs are as-of the event clock, not as-of real time.

---

## Local publishes while replaying

Mixing direct local socket `PUB` traffic with JetStream event-time replay would
make the same patchbay state see two unrelated time sources.

Import/tap mode already has the right invariant:

- JetStream replay/live mode rejects local client `PUB`.
- Local clients may still subscribe to derived output.
- Downstream output remains ordinary monoblok publish/router behavior.

If a future version needs local publishes in this mode, it should define a
single clock policy for them:

- use the current event clock
- require an explicit event-time field
- or route them into a separate processing-time patchbay state

The default should not silently mix clocks.

---

## Output semantics

Replay output should be treated as generated output for the historical event
timeline.

Open questions for a first implementation:

- Should replay output be published to the same subjects as live output, or to a
  replay-prefixed namespace?
- Should LVC update during catch-up, or only after the live handoff?
- Should snapshots taken during catch-up be marked with event clock metadata?
- Should duplicate downstream output be suppressed when replaying from an older
  durable position?

The simplest operational model is:

- run catch-up in a separate environment or namespace when recomputing history
- switch consumers/subscribers to the live output only after catch-up completes
- make output replay behavior explicit in configuration

---

## Ack and failure policy

Acking before patchbay evaluation risks losing a message whose derived output
was not produced. Acking after evaluation preserves replay correctness, but it
means slow output paths can slow the consumer and redelivery is possible on
crash.

The first implementation should:

- ack after successful patchbay evaluation and accepted downstream publish
  attempts
- leave idempotence to the downstream subject design or replay namespace
- expose counters for received, processed, acked, redelivered, failed, and lag
- fail closed on malformed metadata unless an explicit processing-time fallback
  is configured

---

## Implementation shape

Keep this separate from the current core import path.

Likely pieces:

- `pb_program_tick_until(program, router, event_now_ms, event_wall_ms)` helper,
  or a small loop around `pb_program_next_clock_deadline()` and
  `pb_program_tick()`
- JetStream-specific ingress module using `nats.c` JetStream APIs
- durable consumer configuration in patchbay top-level config
- event-time conversion and monotonicity checks
- catch-up high-water tracking
- live handoff state and lag metrics

The important boundary is that patchbay already has the right shape: evaluation
takes `now_ms` and `wall_ms`. The missing piece is an ingress scheduler that can
advance those values from JetStream metadata instead of always using `uv_now()`
and the daemon wall clock.
