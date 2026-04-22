<p align="center">
  <img src="monoblok.png" alt="monoblok" width="480" style="border-radius: 16px; box-shadow: 0 8px 24px rgba(0,0,0,0.25);">
</p>

# monoblok

An experimental toy, this is a partially NATS-compatible pub/sub daemon with last-value streams and an S-expression signal-routing and conditioning DSL called **patchbay**. [Read the introduction post](https://alexjreid.dev/posts/monoblok/)

## Build & run

```
zig build --release=safe
./zig-out/bin/monoblok --port 4222 --patchbay patchbay.edn
```

Builds for the current platform. For other targets or shipping
binaries for several platforms at once, see [Cross-compile](#cross-compile).
Prebuilt Mac (ARM only), Linux and Windows binaries are on the
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

The shipped [patchbay.edn](./patchbay.edn) wires up a handful of forms on `sensors.*`
and `log.app`. Start the daemon, then in another shell subscribe so you
can watch the derived traffic show up:

```
nats sub 'sensors.>'       # in one shell
nats sub 'events.>'        # in another (for the alert rule)
```

1. **`sensors.temp.high`, threshold.** Anything `> 30.0` is mirrored
   onto `.high`:
   ```
   nats pub sensors.temp 42.5
   ```

2. **`events.alerts`, content match.** Any payload containing
   `"alert"` is mirrored onto `events.alerts` with the original subject
   prepended:
   ```
   nats pub log.app "kernel: alert!"
   ```

3. **`sensors.temp.stable`, round + squelch pipeline.** Only emits on
   a change after rounding to 1 decimal. Send five values; you'll see
   three `.stable` messages with payloads `42`, `42.1`, `43`:
   ```
   for v in 42.01 42.04 42.08 42.12 43.00; do
       nats pub sensors.temp "$v"
   done
   ```

4. **`sensors.temp.delta`, analog deadband.** Suppresses changes under
   `0.5`. Send six values; expect three `.delta` emissions (at `10.0`,
   `10.6`, `11.2`; the others are within deadband of the last accepted
   anchor):
   ```
   for v in 10.0 10.2 10.4 10.6 10.7 11.2; do
       nats pub sensors.temp "$v"
   done
   ```

5. **`sensors.temp.smoothed`, 10-sample moving average + deadband
   `1.0`.** Needs a longer stream to drift the mean past each
   threshold:
   ```
   for v in 10 10 10 10 10 10 10 10 10 10 12 12 12 12 12 14 14 14 14 14; do
       nats pub sensors.temp "$v"
   done
   ```

Stateful ops (`squelch`, `deadband`, `moving-*`) keep their state
**per rule, per subject** for the daemon's lifetime; restart the
daemon to reset. The first sample a rule sees on a subject always
passes the gate (no prior value to compare against).

**`$LVC` cache demo.** Publish once, then subscribe late and still
receive the current value:

```
nats pub config.knob hello
nats sub '$LVC.config.knob'      # prints `hello` immediately.. subsequent values as they arrive
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
re-enter the DSL, there are no cycles.

### Wait, signal conditioning?

Borrowed from electronics, where it means cleaning up a raw analog
reading before anything downstream has to deal with it: smoothing
noise, ignoring tiny wobbles, snapping to a grid, suppressing
duplicates. A temperature sensor that reports 22.031, 22.028, 22.034,
22.031 fifty times a second is technically accurate and practically
useless; you want "22.0, and tell me when it actually changes."

patchbay does the software version of that, at the broker, before
your subscribers ever see the message. `round` snaps to decimals,
`quantize` snaps to a step size, `squelch` drops repeats, `deadband`
ignores changes below a threshold, `moving-avg` smooths a window.
Chain them together and a chatty sensor becomes a well-behaved
"change-only" stream, so subscribers don't have to deal with the
noise themselves and can stay simple: they get a clean signal.

### Why the electronics vocabulary

Because the operations already have names, and the names already mean
the right thing. `squelch` on a radio suppresses the channel until
signal strength changes; here it suppresses the message until the
value changes. `deadband` on an industrial controller ignores input
movement smaller than a threshold; here it ignores payload changes
smaller than a threshold. A "patchbay" in a studio is a grid of jacks
you physically wire between sources and destinations, which is exactly
what the DSL looks like on the page, subject filters on the left,
sends on the right. Reaching for "filter chain" or "stream transform"
works, but they're generic names that carry no intuition: a reader
who's seen a hardware squelch knob immediately knows what a
`(squelch payload)` call does, whereas "idempotent publish filter"
has to be explained from scratch. The terms are also specific enough
to resist scope creep, `deadband` is a numeric gate, full stop, so
there's no pressure to overload it into something fancier.

It's also just more fun than calling everything `FilterOperatorImpl`.

### What it can be used for

Filtering, routing, light payload rewriting, and signal conditioning
at the broker. A form inspects an incoming message and can `publish`
zero or more derived messages on other subjects; think "threshold
this numeric stream onto a `.high` sub-subject," "mirror anything
mentioning `alert` into `events.alerts`," "split a firehose into
per-tenant subjects," or "deadband a jittery sensor so only meaningful
changes hit downstream." The stateful primitives (`squelch`,
`deadband`, `moving-*`) keep O(1) per-`(rule, subject)` state but
there's no time-based windowing, no aggregation across messages, no
storage. It's not a stream processor.

```edn
(on "sensors.*"
  (when (> payload-float 30.0)
    (publish (subject-append "high") payload)))

(on ">"
  (when (contains? payload "alert")
    (publish "events.alerts" (str-concat subject ": " payload))))
```

### What about NEX and similar?

[NEX](https://github.com/synadia-io/nex) (the NATS Execution Engine) is a
workload scheduler: it runs JS functions, WebAssembly modules, or static
Linux binaries on agents next to a NATS cluster, each with its own process,
sandbox, and scoped NATS credentials. Triggers are general (pub/sub,
request/reply, JetStream), and the function can do arbitrary work: HTTP
calls, database writes, anything.

patchbay is not that. It's an in-broker routing DSL evaluated on the event
loop, no processes, no sandbox, no NATS client. The only side effect a rule
can have is `publish`; bodies are pure over `subject` / `payload` /
`payload-float`. It's for per-message transform and gating (quantize,
deadband, squelch, moving averages, re-publish to another subject) without
leaving the broker.

Look at NEX when the work is arbitrary code with external I/O. Look at
patchbay when the work is "reshape the message and maybe drop it" and you
don't want a second process in the path. They compose fine: a NEX function
can subscribe to the cleaned-up stream a patchbay rule is producing.

### Not NATS subject mappings either

nats-server has built-in [subject mappings / transforms](https://docs.nats.io/nats-concepts/subject_mapping)
configured in the server config file: pattern-match an incoming subject
with wildcards and rewrite it, e.g. `"bar.*.*" : "baz.{{wildcard(2)}}.{{wildcard(1)}}"`
to swap tokens, plus partitioning helpers for splitting a firehose across
queue workers. JetStream `republish` and stream `subject_transform` use
the same rewrite syntax.

That's pure subject rewriting: the rewritten subject still carries the
original payload, and there's no condition on the payload, no numeric
comparison, no state across messages. patchbay can do subject rewriting
too (via `subject-append` and `publish` to any subject), but the thing it
exists for is looking at `payload-float`, gating on value change, and
maintaining per-(rule, subject) state (`squelch`, `deadband`, `moving-avg`).
If all you want is "rename `bar.a.b` to `baz.b.a`," the built-in mappings
are what should be used.

### Reference

The full DSL reference (syntax, bound symbols, every operator, all the
tables and worked pipelines) lives in [PATCHBAY.md](./PATCHBAY.md).

## `$LVC.*`: last-value stream

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

## `$STATS.*`: live counters

The server publishes cumulative counters to `$STATS.*` on a 1-second
wall-clock tick. Values are u64 decimals; subscribers compute their own
rates across ticks. The stream goes through normal fan-out, so it's
cached by LVC and `SUB $LVC.$STATS.>` gives the current value on
subscribe.

| subject                          | value                                             |
|----------------------------------|---------------------------------------------------|
| `$STATS.global.pubs`             | total inbound client PUBs since server start      |
| `$STATS.rules.<i>.emitted`       | successful `publish` / `publish-to` calls by rule |
| `$STATS.rules.<i>.suppressed`    | gate suppressions (squelch, deadband, changed?, rising-edge, falling-edge) by rule |

Rules are indexed by position in the patchbay file, 0-based. Client
publishes to `$STATS.*` are rejected (read-only, like `$LVC.*`).

```
SUB $LVC.$STATS.rules.0.suppressed   ; -> immediately receives current count
SUB $STATS.global.pubs               ; -> live ticks every second
```

## Observability

Two always-on warn thresholds fire if something's off:

- **Patchbay amplification**: a single inbound PUB that causes 64+
  rule-generated publishes logs once with the offending subject.
  Catches runaway fan-out from a misconfigured rule.
- **Outbound buffer high-water**: if any connection's pending-write
  buffer grows past 4 MiB between drains, logs the conn id and the
  size. Usually means a slow consumer or a fan-out target that
  stopped reading.

`--stats` opts into a periodic summary line every 10k inbound PUBs,
with the observed max rule-publishes-per-input and max per-conn
outbound buffer over the window:

```
info: stats: pubs=10000 max_rule_publishes=0 max_out_hwm=41160B
```

Useful for seeing how much headroom you have under the warn thresholds.
No output when `--stats` is off.

## Architecture

One `xev.Loop` owns accept, per-connection read/write completions,
router state, and the LVC. No mutexes, no atomics on the hot path.
Fan-out appends bytes directly to each subscriber's outbound
`ArrayList` and kicks a single `write` per connection per publish,
with partial-write handling.

### Single-threaded, on purpose

Everything application-level runs on a single thread: parsing, subject
matching, rule evaluation, fan-out, write buffering. The kernel still
gets to use your other cores for actual I/O, but once a byte arrives
it's single file through monoblok. No thread pool. No work queue.
One thread.

That's a deliberate choice, not a TODO. It's what lets the whole thing skip locks
entirely, keep refcounts as plain `u32`s, reuse LVC buffers in place
instead of reallocating, and have fan-out alias directly into caller
buffers without copying. The moment you add a second thread, every one
of those shortcuts turns into a bug.

The trade is a one-core ceiling. A heavy rule on a hot subject, or a
giant fan-out, will stall every other connection while it runs.
monoblok is aimed at signal-conditioning patchbays sitting in front
of modest pub/sub traffic, which fits comfortably inside one core.
If you outgrow that, the answer isn't threading the loop, it's
**sharding**: N loops - more later.

## Tests

```
zig build test              # unit tests
bash scripts/smoke.sh       # end-to-end over raw TCP
bash scripts/bench.sh       # pub + fan-out bench (needs `nats` CLI)
```

## Benchmarks

Fair warning before the numbers: `nats-server` is a mature, battle-tested
Go codebase with a decade of production history behind it. It's doing
substantially more than monoblok: accounting, logging, metrics, slow-consumer
detection, account isolation, clustering, JetStream, TLS, auth, any of
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
generator. Single run each: indicative, not rigorous. monoblok built
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
(their sublist is a token-keyed radix tree, ours is linear).
`--release=fast` gains roughly 10–15% if you want to poke at the ceiling.

### M4 Mac Mini

M4 Mac Mini (10-core, 16 GB), macOS 26.2, Zig 0.16, libxev kqueue backend,
vs `nats-server` v2.12.7. ReleaseSafe.

| workload             |   monoblok | nats-server |       Δ |
|----------------------|-----------:|------------:|--------:|
| 1 pub × 500k × 64B   |  6.46M/s   |   7.14M/s   |    −9%  |
| 2 pub × 10k × 64B    |  7.35M/s   |   5.97M/s   |  **+23%** |
| 8 pub × 50k × 128B   | 11.83M/s   |   8.17M/s   |  **+45%** |
| 1 pub → 1 sub        |  4.27M/s   |   3.93M/s   |   **+9%** |
| 1 pub → 10 subs      | 12.60M/s   |   4.95M/s   | **+155%** |
| 1 pub → 50 subs      | 17.52M/s   |   4.82M/s   | **+264%** |

Fanout shows the largest delta, though see the caveat above about what
nats-server is doing that monoblok isn't.

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
io_uring works cleanly end-to-end. Single-subscriber fan-out is weak;
the 1-sub workload is dominated by scheduling on a 2-vCPU box and our
profile differs from nats-server's there; ~10 subs onward it levels
out.

## Why libxev

Zig 0.16's `std.Io` works, but not in a way that fits a single-threaded
event loop: the macOS `Dispatch` backend is thread-per-connection, and
the other backends I tried (`Kqueue`, `Uring`) didn't compile cleanly
for me on 0.16 (could well be me holding it wrong, the API is still
shifting). libxev gives us a proper single-loop model on
kqueue/io_uring/epoll/IOCP, so that's what we use. It's great.

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

macOS arm64 binaries are built natively on a macOS runner during the
release workflow (see below), not via `zig build dist`.

Pick one and `scp` it anywhere. The Linux binaries are statically
linked against musl so there's no glibc dependency. Easy enough to
add to a tiny container image if that's your jam.

For an ad-hoc one-off target that isn't in the dist set, the vanilla
Zig flag still works:

```
zig build --release=safe -Dtarget=x86_64-linux-gnu
```

libxev picks the right backend at comptime: `io_uring` on Linux,
`kqueue` on macOS, `iocp` on Windows. The daemon logs which backend
it's using at startup.

## License

MIT. See `LICENSE`.
