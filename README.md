<p align="center">
  <img src="monoblok.png" alt="monoblok" width="480" style="border-radius: 16px; box-shadow: 0 8px 24px rgba(0,0,0,0.25);">
</p>

# monoblok

An experimental, partially NATS-compatible pub/sub daemon with last-value streams and an S-expression signal-routing DSL called **patchbay**.

## Build & run

```
zig build --release=safe
./zig-out/bin/monoblok --port 4222 --patchbay patchbay.edn
```

Builds for the current platform. For other targets or shipping
binaries for several platforms at once, see [Cross-compile](#cross-compile).
Prebuilt Linux and Windows binaries are on the
[latest release page](https://github.com/lexvicacom/monoblok/releases/latest).

Any NATS client works:

```
nats pub sensors.temp 42.5
nats sub 'sensors.*'
```

Subjects are alphanumerics plus `- _ $`; `*` is a single token, `>` is
a trailing wildcard. Core protocol only: no auth, TLS, queue groups,
headers or JetStream.

### Driving the demo patchbay

The shipped `patchbay.edn` wires up a handful of forms on `sensors.*`
and `log.app`. Start the daemon, then in another shell subscribe so you
can watch the derived traffic show up:

```
nats sub 'sensors.>'       # in one shell
nats sub 'events.>'        # in another (for the alert rule)
```

1. **`sensors.temp.high` — threshold.** Anything `> 30.0` is mirrored
   onto `.high`:
   ```
   nats pub sensors.temp 42.5
   ```

2. **`events.alerts` — content match.** Any payload containing
   `"alert"` is mirrored onto `events.alerts` with the original subject
   prepended:
   ```
   nats pub log.app "kernel: alert!"
   ```

3. **`sensors.temp.stable` — round + squelch pipeline.** Only emits on
   a change after rounding to 1 decimal. Send five values; you'll see
   three `.stable` messages with payloads `42`, `42.1`, `43`:
   ```
   for v in 42.01 42.04 42.08 42.12 43.00; do
       nats pub sensors.temp "$v"
   done
   ```

4. **`sensors.temp.delta` — analog deadband.** Suppresses changes under
   `0.5`. Send six values; expect three `.delta` emissions (at `10.0`,
   `10.6`, `11.2` — the others are within deadband of the last accepted
   anchor):
   ```
   for v in 10.0 10.2 10.4 10.6 10.7 11.2; do
       nats pub sensors.temp "$v"
   done
   ```

5. **`sensors.temp.smoothed` — 10-sample moving average + deadband
   `1.0`.** Needs a longer stream to drift the mean past each
   threshold:
   ```
   for v in 10 10 10 10 10 10 10 10 10 10 12 12 12 12 12 14 14 14 14 14; do
       nats pub sensors.temp "$v"
   done
   ```

Stateful ops (`squelch`, `deadband`, `moving-*`) keep their state
**per rule, per subject** for the daemon's lifetime — restart the
daemon to reset. The first sample a rule sees on a subject always
passes the gate (no prior value to compare against).

**`$LVC` cache demo** — publish once, then subscribe late and still
receive the current value:

```
nats pub config.knob hello
nats sub '$LVC.config.knob'      # prints `hello` immediately
```

`knob` LOL.

## Patchbay

patchbay is a small S-expression DSL describing how every incoming
publish gets filtered, conditioned, and re-routed. Think of it as a
wiring diagram: jacks (subject filters) at the top, filter chains in
the middle (`round`, `squelch`, `deadband`, `moving-avg` …), and sends
(`publish-to`) at the bottom. Top-level forms are
`(on SUBJECT-FILTER BODY)`; `BODY` is evaluated whenever an incoming
subject matches `SUBJECT-FILTER` (normal `*` / `>` wildcards can apply).
Messages a patchbay form publishes fan out normally but **do not**
re-enter the DS, there are no cycles.

### What it can be used for

Filtering, routing, light payload rewriting, and signal conditioning
at the broker. A form inspects an incoming message and can `publish`
zero or more derived messages on other subjects — think "threshold
this numeric stream onto a `.high` sub-subject," "mirror anything
mentioning `alert` into `events.alerts`," "split a firehose into
per-tenant subjects," or "deadband a jittery sensor so only meaningful
changes hit downstream." The stateful primitives (`squelch`,
`deadband`, `moving-*`) keep O(1) per-`(rule, subject)` state but
there's no time-based windowing, no aggregation across messages, no
storage. It's not a stream processor and not Turing-complete — closer
to a signal-chain / patchbay with a little arithmetic and string glue
than to a full CEP engine.

```edn
(on "sensors.*"
  (when (> payload-float 30.0)
    (publish (subject-append "high") payload)))

(on ">"
  (when (contains? payload "alert")
    (publish "events.alerts" (str-concat subject ": " payload))))
```

### Values

`nil`, booleans (`true` / `false`), numbers (parsed as `f64`),
strings (`"..."`), symbols, and lists. Truthiness: `nil` and `false`
are falsy, everything else (including `0` and `""`) is truthy.

### Bound symbols

Bare symbols inside a body that resolve against the current message:

| symbol          | type     | value                              |
|-----------------|----------|------------------------------------|
| `subject`       | string   | the incoming subject               |
| `payload`       | string   | the raw payload bytes              |
| `payload-float` | number   | `payload` parsed as a float (errors if not numeric) |

### Special forms

Evaluate their arguments lazily / with short-circuiting.

| form                        | behavior                                                     |
|-----------------------------|--------------------------------------------------------------|
| `(if C T E?)`               | `T` if `C` is truthy, else `E` (or `nil` if omitted)         |
| `(when C BODY...)`          | evaluate `BODY` sequentially iff `C` is truthy               |
| `(and X...)`                | left-to-right, returns the first falsy value or the last     |
| `(or  X...)`                | left-to-right, returns the first truthy value or the last    |
| `(do  X...)`                | evaluate in order, return the last                           |
| `(-> X F...)`               | thread `X` as the **last** arg of each `F` (see below)       |

### Comparisons and logic

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

### Arithmetic

Variadic, left-fold, numeric. Single-arg variants follow Clojure:
`(- x)` negates, `(/ x)` reciprocates, `(+ x)` / `(* x)` are identity.

`(+ a b ...)` `(- a b ...)` `(* a b ...)` `(/ a b ...)`

### Strings and subjects

| form                             | result                                              |
|----------------------------------|-----------------------------------------------------|
| `(str-concat a b ...)`           | concatenates string/symbol args                     |
| `(subject-append "suffix")`      | `"<current-subject>.suffix"`                        |
| `(contains? haystack needle)`    | boolean substring check                             |

### Side effects

`(publish SUBJECT PAYLOAD)` — validates `SUBJECT` as a publishable
subject (no wildcards, no `$LVC.*`) and enqueues a fan-out. Returns
`nil`. Publishes from rules participate in normal delivery + LVC
caching but are not themselves fed back through rule evaluation.

`(publish-to SUBJECT VALUE)` — same thing with args flipped so it
slots on the tail of a `->` pipeline. Coerces numeric `VALUE` to its
canonical string form. If `VALUE` is `nil` — which is what a gate
returns when it suppresses — `publish-to` is a no-op. That nil
short-circuit is what makes the pipeline form below read
top-to-bottom.

### Threading with `->`

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

### Idempotent filters

Real sensor streams are noisy. These four primitives turn a chatty
publisher into a "change-only" one without any timers or windows —
one slot of state per subject the rule has ever seen.

| form                  | behavior                                                                       |
|-----------------------|--------------------------------------------------------------------------------|
| `(round N X)`         | round number `X` to `N` decimal places (pure)                                  |
| `(quantize STEP X)`   | snap `X` to the nearest multiple of `STEP` (pure)                              |
| `(squelch X)`         | pass `X` through iff it differs from the last `X` seen on this (rule, subject) |
| `(deadband DELTA X)`  | pass `X` through iff numeric `X` changed by at least `DELTA` since last emit   |

Gates return the value on pass and `nil` on suppress — truthy-on-pass
keeps them usable as conditions in `when` / `and`, while passing the
value through makes them compose directly with `->` and `publish-to`.
`squelch` stores the stringified value; `deadband` stores the numeric
anchor and only updates it on an accepted change. Both are **per rule,
per subject** — two rules watching the same subject don't interfere,
and the first message a rule sees on a new subject always passes.

```edn
; Jittery temperature sensor → only emit when the rounded value moves.
(on "sensors.*"
  (-> payload-float
      (round 1)
      (squelch)
      (publish-to (subject-append "stable"))))

; Analog deadband — suppress changes below 0.5.
(on "sensors.*"
  (-> payload-float
      (deadband 0.5)
      (publish-to (subject-append "delta"))))
```

### Windowed aggregates

A small ring-buffer family for smoothing and window-based triggers.
Each call site gets its own N-wide ring, keyed by `(rule, subject, op)`
— so `(moving-avg 10 ...)` and `(moving-max 10 ...)` in the same rule
maintain independent windows.

| form                 | returns | cost                                    |
|----------------------|---------|-----------------------------------------|
| `(moving-avg N X)`   | number  | O(1) per update (running sum)           |
| `(moving-sum N X)`   | number  | O(1)                                    |
| `(moving-max N X)`   | number  | O(1) amortized (monotonic deque)        |
| `(moving-min N X)`   | number  | O(1) amortized                          |

Memory is `N × 8 B` per `(rule, subject, op)` slot plus a small deque
for max/min. `N` is fixed at first call — don't change it between
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
comparison, and gating primitives above — no special pipeline syntax.

## `$LVC.*` — last-value stream

Every subject has an implicit last-value cache. Subscribing to
`$LVC.foo.bar` joins a live stream of `foo.bar`: current cached value
first (if any), then every subsequent publish. Wildcards work.
`PUB $LVC.*` is rejected.

```
PUB foo.bar 11      ; cache = 11
PUB foo.bar 12      ; cache = 12
SUB $LVC.foo.bar    ; -> immediately receives 12
PUB foo.bar 13      ; -> subscriber receives 13
```

On by default; `--no-lvc` disables (~2–4% overhead when enabled).

## Architecture

One `xev.Loop` owns accept, per-connection read/write completions,
router state, and the LVC. No mutexes, no atomics on the hot path.
Fan-out appends bytes directly to each subscriber's outbound
`ArrayList` and kicks a single `write` per connection per publish,
with partial-write handling.

See `CLAUDE.md` for module-level detail.

## Tests

```
zig build test              # unit tests
bash scripts/smoke.sh       # end-to-end over raw TCP
bash scripts/bench.sh       # pub + fan-out bench (needs `nats` CLI)
```

## Benchmarks

Fair warning before the numbers: `nats-server` is a mature, battle-tested
Go codebase with a decade of production history behind it. It's doing
substantially more than monoblok — accounting, logging, metrics, slow-consumer
detection, account isolation, clustering, JetStream, TLS, auth — any of
which has a non-zero runtime cost even when unused. monoblok is a
comparative toy implementing a tiny slice of that. Treat these numbers as
informational, not as a claim of "faster than nats-server."

Also: **monoblok was benchmarked with an empty patchbay** (no
`--patchbay` flag). The numbers measure raw PUB/SUB + fan-out; they
say nothing about rule-evaluation cost. A realistic patchbay adds
work on every matching publish (s-expr tree walk, arena allocations,
stateful-op hashmap lookups), so expect throughput to drop from these
figures in proportion to how much your rules do.

M2 MacBook Air (8-core, 16 GB), Zig 0.16, libxev kqueue backend,
vs `nats-server` 2.9.6 on the same machine. `nats bench` as the load
generator. Single run each — indicative, not rigorous. monoblok built
with `--release=safe` (what `zig build dist` produces).

Publish-only:

| workload | monoblok | nats-server |
|---|---:|---:|
| 1 × 500k × 64B | 6.12M msg/s | 6.18M msg/s |
| 2 × 10k × 64B | 4.57M msg/s | 5.19M msg/s |
| 8 × 50k × 128B | 4.64M msg/s | 10.29M msg/s |

Fan-out (1 pub, N subs, aggregated sub rate):

| subs | monoblok | nats-server |
|---|---:|---:|
| 1 | 2.04M msg/s | 2.99M msg/s |
| 10 | 7.03M msg/s | 6.76M msg/s |
| 50 | 8.01M msg/s | 6.70M msg/s |

Single-publisher parity; multi-publisher nats-server pulls ahead
(their sublist is a token-keyed radix tree, ours is linear); fan-out
we still edge out at 10+ subs. `--release=fast` gains roughly 10-15%
on fan-out if you want to see the ceiling.

### Linux

Ubuntu 24.04 (2-core AMD EPYC KVM VM, 4 GB), Zig 0.16, libxev
io_uring backend, vs `nats-server` v2.12.7 on the same machine.
ReleaseSafe (via `zig build dist`). I think this is the second cheapest machine Hetzner sell.

Publish-only:

| workload | monoblok | nats-server |
|---|---:|---:|
| 1 × 500k × 64B | 2.77M msg/s | 3.79M msg/s |
| 2 × 10k × 64B | 2.74M msg/s | 2.38M msg/s |
| 8 × 50k × 128B | 3.40M msg/s | 3.50M msg/s |

Fan-out:

| subs | monoblok | nats-server |
|---|---:|---:|
| 1 | 0.65M msg/s | 1.57M msg/s |
| 10 | 2.87M msg/s | 3.01M msg/s |
| 50 | 4.51M msg/s | 3.28M msg/s |

Numbers are lower than the M2 MBA as expected for a 2-vCPU cheap cloud VM.
io_uring works cleanly end-to-end. Single-subscriber fan-out is weak —
the 1-sub workload is dominated by scheduling on a 2-vCPU box and our
profile differs from nats-server's there; ~10 subs onward it levels
out.

## Why libxev

Zig 0.16's `std.Io` networking backends are broken on every target we
tried: `Dispatch` (macOS) has no net ops, `Kqueue` references a vtable
field that no longer exists, `Uring` has error-set mismatches. libxev
has working kqueue/io_uring/epoll/IOCP, so that's what we use. It's great.

## Cross-compile

Zig cross-compiles out of the box. The `dist` build step produces
ReleaseSafe binaries for common targets into `dist/<triple>/`,
alongside `patchbay.edn` and the bench script:

```
zig build dist
# dist/x86_64-linux-musl/   → monoblok (static musl ELF)
# dist/aarch64-linux-musl/  → monoblok (static musl ELF, ARM64)
# dist/x86_64-windows-gnu/  → monoblok.exe
```

Pick one and `scp` it anywhere — the Linux binaries are statically
linked against musl so there's no glibc dependency.

For an ad-hoc one-off target that isn't in the dist set, the vanilla
Zig flag still works:

```
zig build --release=safe -Dtarget=x86_64-linux-gnu
```

libxev picks the right backend at comptime: `io_uring` on Linux,
`kqueue` on macOS, `iocp` on Windows. The daemon logs which backend
it's using at startup.

## Releases

Pushing a tag matching `v[0-9]*` (e.g. `v0.1.0`) triggers the release
workflow in `.github/workflows/release.yml`: it runs tests, runs
`zig build dist`, packages each target (`tar.gz` for Linux,
`zip` for Windows), and attaches them to a new GitHub release.

Only Linux (`x86_64`, `aarch64`) and Windows (`x86_64`) binaries are
shipped. macOS is not included — unsigned Mac binaries hit Gatekeeper
warnings and want an `xattr -d com.apple.quarantine` dance, which is
more friction than just building locally. If you're on a Mac:
`zig build --release=safe`.

## License

MIT. See `LICENSE`.
