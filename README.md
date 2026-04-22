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

Builds for the current platform. For how release binaries are produced,
see [Building for release](#building-for-release). Prebuilt Mac (ARM
only) and Linux (x86_64 + aarch64) binaries are on the
[latest release page](https://github.com/lexvicacom/monoblok/releases/latest).
Each platform ships two tarballs: the default (includes the
[outbound NATS bridge](#outbound-nats-bridge), needs OpenSSL on the
target box) and a `-nobridge` variant with no external runtime
dependencies — grab whichever fits your use case.

### Dependencies

OpenSSL (dynamically linked) is required when the NATS bridge is enabled,
which is the default. Install it with:

```
# macOS
brew install openssl@3

# Debian / Ubuntu
sudo apt install libssl-dev pkg-config

# Fedora / RHEL
sudo dnf install openssl-devel pkgconf-pkg-config

# Arch
sudo pacman -S openssl pkgconf
```

If you don't want the bridge (or don't want to install OpenSSL), build with
`zig build -Dbridge=false`.

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

### What it isn't

[NEX](https://github.com/synadia-io/nex) runs arbitrary code (JS, Wasm,
binaries) on agents next to the cluster, with HTTP, DB, anything. patchbay
is an in-broker DSL with no processes and no sandbox, where the only side
effect is `publish`; reach for NEX when you need external I/O.

nats-server's built-in [subject mappings](https://docs.nats.io/nats-concepts/subject_mapping)
rewrite subjects with wildcards but can't see the payload or keep state. If
all you want is "rename `bar.a.b` to `baz.b.a`," use those; patchbay is for
gating on `payload-float` and per-(rule, subject) state (`squelch`,
`deadband`, `moving-avg`).

### Reference

The full DSL reference (syntax, bound symbols, every operator, all the
tables and worked pipelines) lives in [PATCHBAY.md](./PATCHBAY.md).

### Using with Claude Code

[CLAUDE_PATCHBAY.md](./CLAUDE_PATCHBAY.md) is a self-contained system
prompt that teaches Claude the DSL. Append it to your project's
`CLAUDE.md` so Claude Code picks it up automatically when editing
`.edn` rule files:

```sh
# project-scoped (recommended)
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/CLAUDE_PATCHBAY.md >> ./CLAUDE.md

# or user-global
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/CLAUDE_PATCHBAY.md >> ~/.claude/CLAUDE.md
```

For one-off use, reference it inline in a prompt with
`@CLAUDE_PATCHBAY.md` (Claude Code inlines `@path` refs).

Once it's loaded, describe the stream you have and the stream you
want. For example:

> Write a patchbay rule file `ticker.edn` for a noisy market ticker on `MARKET.<SYM>`.
> Round the price to 3 decimal places and only re-emit when the
> rounded value changes. Also fan out big jumps to an alerts subject,
> and bridge those alerts out to a real NATS server at
> `nats://127.0.0.1:4222`.

<p align="center">
  <img src="claude.png" alt="Claude Code editing a patchbay" width="720">
</p>

Drop `ticker.edn` into a convenient place and run it with `monoblock --patchbay ticker.edn`

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

The server publishes cumulative counters to `$STATS.*` on a 1-minute
wall-clock tick. Values are u64 decimals; subscribers compute their own
rates across ticks. The stream goes through normal fan-out, so it's
cached by LVC and `SUB $LVC.$STATS.>` gives the current value on
subscribe.

| subject                          | value                                             |
|----------------------------------|---------------------------------------------------|
| `$STATS.global.pubs`             | total inbound client PUBs since server start      |
| `$STATS.rules.<i>.emitted`       | successful `publish` / `publish-to` calls by rule |
| `$STATS.rules.<i>.suppressed`    | gate suppressions (squelch, deadband, changed?, rising-edge, falling-edge) by rule |
| `$STATS.bridge.published`        | publishes forwarded to the remote NATS cluster (if the bridge is configured) |
| `$STATS.bridge.dropped`          | publishes the bridge failed to forward (connection closed, subject too long, remote error) |

Rules are indexed by position in the patchbay file, 0-based. Client
publishes to `$STATS.*` are rejected (read-only, like `$LVC.*`).

```
SUB $LVC.$STATS.rules.0.suppressed   ; -> immediately receives current count
SUB $STATS.global.pubs               ; -> live ticks every second
```

## Outbound NATS bridge

monoblok can forward a subset of local publishes to a real NATS cluster,
so it can sit in front of (or alongside) a NATS deployment and hand off
selected traffic. **Export-only**: nothing flows in from the remote.
TLS and `.creds` files are supported; the upstream connection uses
vendored [nats.c](https://github.com/nats-io/nats.c) and dyn-linked
OpenSSL.

Typical shape:

- Local clients publish to short, cheap subjects on monoblok.
- Patchbay rules condition the signal (round, deadband, squelch, moving
  averages, etc.).
- The bridge forwards only the subjects you want (raw sensor reads, or
  just the derived stable ones, or both) to a remote NATS cluster —
  Synadia NGS, a managed cluster, a self-hosted cluster, etc.
- Nothing else leaves the box. Everything off-path stays local.

### Config

Zero or one `(bridge ...)` form in the patchbay file configures it:

```
(bridge
  :servers  ("tls://connect.ngs.global:4222")
  :creds    "/etc/monoblok/ngs.creds"
  :tls      true
  :name     "monoblok-prod-1"
  :export   ("telemetry.>" "alerts.>"))
```

Full keyword reference:

| keyword                     | type            | notes                                             |
|-----------------------------|-----------------|---------------------------------------------------|
| `:servers`                  | list of strings | **required**. `nats://` or `tls://` URLs.         |
| `:export`                   | list of strings | subject filters (wildcards allowed). Local publishes matching any of these are forwarded. |
| `:name`                     | string          | client name shown in the remote's monitoring      |
| `:creds`                    | path            | JWT + NKey credentials file (NGS / operator mode) |
| `:user` / `:password`       | string          | basic auth                                        |
| `:token`                    | string          | bearer-token auth                                 |
| `:tls`                      | bool            | enable TLS. Required if any `:servers` URL is `tls://`. |
| `:tls-ca`                   | path            | CA bundle (PEM)                                   |
| `:tls-cert` / `:tls-key`    | path            | client cert + private key (mTLS)                  |
| `:tls-skip-verify`          | bool            | **dev only**. Accepts any server cert.            |
| `:connect-timeout-ms`       | number          | initial-connect timeout                           |
| `:ping-interval-ms`         | number          | keepalive ping cadence                            |
| `:max-reconnect`            | number          | `-1` for unlimited                                |
| `:reconnect-wait-ms`        | number          | base delay between reconnect attempts             |

Auth precedence: `:creds` > `:user`/`:password` > `:token`. If TLS is on
but `:tls-ca` isn't set, nats.c falls back to the system trust store.

### Semantics

A local publish (from a NATS client or from a patchbay rule) whose
subject matches **any** `:export` filter is forwarded to the remote
as-is. Subjects that don't match any filter never leave the daemon.
Fan-out is: local subscribers served first, bridge second — so a slow
or reconnecting remote can't starve local delivery.

Reconnects are handled by nats.c internally. During the reconnect
window, publishes are buffered up to the library default; once the
buffer is full, further publishes count as dropped.

### Counters

Published on the `$STATS.*` tick (1/sec):

| subject                          | value                                                |
|----------------------------------|------------------------------------------------------|
| `$STATS.bridge.published`        | successful `natsConnection_Publish` calls            |
| `$STATS.bridge.dropped`          | failures: remote closed, buffer full, subject >=512B |

### Disabling

The bridge is on by default. Turn it off at build time with
`zig build -Dbridge=false`, which also removes the OpenSSL link
dependency. Both variants are published to each release as `-nobridge`
and (default) tarballs — pick whichever fits the target box.

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

`nats-server` is a mature Go codebase doing a lot more than monoblok
(accounting, metrics, slow-consumer detection, clustering, JetStream,
TLS, auth). These numbers are informational, not a "faster than
nats-server" claim. monoblok is benchmarked with an **empty patchbay**,
so this is raw PUB/SUB + fan-out only; a real patchbay adds work per
matching publish.

Both columns are msgs/sec from `nats bench`, single run each. monoblok
built `--release=safe`, vs `nats-server` v2.12.7.

`scripts/bench.sh` drives the numbers: it starts monoblok on `$NATS_URL`
(default `127.0.0.1:4222`), runs the six `nats bench` workloads in the
table below against it, stops it, then (if `nats-server` is on `PATH`)
starts it on the same port and reruns the same workloads. Servers are
benched sequentially, never concurrently. Pub workloads use `nats bench
pub`, fan-out workloads spawn a `nats bench sub` with N clients and
then a single publisher on the same subject. The script scrapes the
`publisher stats` / `subscriber stats` line from each run and prints
the msgs/sec table at the end.

**M4 Mac Mini** (10-core, 16 GB, macOS 26.2, kqueue) vs **Hetzner Linux**
(2-core AMD EPYC KVM, 4 GB, Ubuntu 24.04, io_uring):

| workload            |    M4 monoblok |     M4 nats |   M4 Δ | Linux monoblok | Linux nats | Linux Δ |
|---------------------|---------------:|------------:|-------:|---------------:|-----------:|--------:|
| 1 pub × 500k × 64B  |       6.46M/s  |    7.14M/s  |    −9% |       3.66M/s  |   3.39M/s  |     +8% |
| 2 pub × 10k × 64B   |       7.35M/s  |    5.97M/s  |   +23% |       3.08M/s  |   3.09M/s  |     −0% |
| 8 pub × 50k × 128B  |      11.83M/s  |    8.17M/s  |   +45% |       2.83M/s  |   2.90M/s  |     −2% |
| 1 pub → 1 sub       |       4.27M/s  |    3.93M/s  |    +9% |       0.73M/s  |   1.09M/s  |    −33% |
| 1 pub → 10 subs     |      12.60M/s  |    4.95M/s  |  +155% |       3.31M/s  |   2.59M/s  |    +28% |
| 1 pub → 50 subs     |      17.52M/s  |    4.82M/s  |  +264% |       3.98M/s  |   3.05M/s  |    +30% |

Fan-out is where monoblok pulls ahead on both platforms. The single-
subscriber workload is the one regression that flips sign between
platforms (likely io_uring completion batching behaving differently
under low concurrency). Multi-publisher wins on the M4 collapse to
parity on the 2-vCPU box, since a single-threaded loop can't scale
past one core while nats-server spreads across both.
`--release=fast` adds roughly 10–15% on top if you want to poke the
ceiling.

## Why libxev

Zig 0.16's `std.Io` works, but not in a way that fits a single-threaded
event loop: the macOS `Dispatch` backend is thread-per-connection, and
the other backends I tried (`Kqueue`, `Uring`) didn't compile cleanly
for me on 0.16 (could well be me holding it wrong, the API is still
shifting). libxev gives us a proper single-loop model on
kqueue/io_uring/epoll/IOCP, so that's what we use. It's great.

## Building for release

Release binaries are built natively on each target architecture, not
cross-compiled. The `.github/workflows/release.yml` pipeline uses three
runners: `ubuntu-22.04` (x86_64), `ubuntu-22.04-arm` (aarch64), and
`macos-latest` (aarch64). Each runs `zig build --release=safe` with
OpenSSL installed locally. The binaries dyn-link against the target's
glibc + OpenSSL, so a reasonably current distro (Debian 12+, Ubuntu
22.04+, any current macOS) is required. glibc 2.35 is the floor — that's
Ubuntu 22.04's libc.

Native per-arch rather than cross-compile is a deliberate choice: it
dodges the pain of sourcing target-OS OpenSSL headers/libs on a foreign
build host, and the GHA runners are free.

Each release ships **two variants per platform**: with the NATS bridge
(requires OpenSSL on the target box) and without (`-nobridge` suffix, no
runtime deps). Grab the bridge variant if you want to forward traffic to
a real NATS cluster; grab the `-nobridge` variant if you just want a
standalone pub/sub broker with no external deps.

To build locally for your own machine, just `zig build --release=safe`
(or `-Dbridge=false` to skip the OpenSSL dependency).

libxev picks the right backend at comptime: `io_uring` on Linux,
`kqueue` on macOS. The daemon logs which backend it's using at startup.

## License

MIT. See `LICENSE`.
