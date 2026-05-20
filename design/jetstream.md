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
  :catch-up true)
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

The current `:subject` option remains useful as the filter subject list for the
JetStream consumer. If JetStream filter semantics cannot match the configured
list exactly, validation should fail rather than silently widening the import.

The consumer name should be stable across restarts for normal catch-up/live
operation. Ephemeral consumers are useful for ad hoc replay, but a durable name
is the safer default for a daemon expected to resume after a crash.

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

Mixing direct local socket `PUB` traffic with JetStream event-time replay is
dangerous because the same patchbay state would see two unrelated time sources.

The first implementation should choose one simple policy:

- JetStream replay/live mode rejects local client `PUB`, similar to import mode.
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
