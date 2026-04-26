<p align="center">
  <img src="monoblok.png" alt="monoblok" width="480" style="border-radius: 16px; box-shadow: 0 8px 24px rgba(0,0,0,0.25);">
</p>

# monoblok

An experimental, partially NATS-compatible pub/sub server written in Zig, with last-value streams and a routing and signal conditioning DSL called **patchbay**. [Read the introductory blog post](https://alexjreid.dev/posts/monoblok/).

monoblok is a single small binary. Local clients publish to short subjects; patchbay rules condition the signal (round, deadband, squelch, moving-avg) and re-emit on derived subjects, so subscribers don't each re-implement the same rounding, deduping, and smoothing. JSON-emitting devices are first-class too: a frame like `{"temp":12.5,"hum":80}` can be [demuxed onto scalar sub-subjects](#json-frames) and then conditioned the same way. It uses the NATS wire protocol, so any `nats` client works, and it's happy [sitting at the edge in front of a NATS leaf](#what-it-can-be-used-for) so the cleaned-up streams roll into your existing cluster.

## Try it out with no install

A public demo server runs at `nats://monoblok.rtd.pub:4222` (no auth, no TLS). Point any `nats` CLI at it and start publishing. See [docs/demo.md](./docs/demo.md) for the loaded patchbay, subjects worth subscribing to, and the usual caveats (tiny server, shared, no rate limiting, don't send secrets).

## Install on your hardware

Prebuilt Mac (Apple Silicon) and Linux (x86_64, aarch64) binaries are on the [latest release page](https://github.com/lexvicacom/monoblok/releases/latest). Each platform ships two `.tar.gz` archives: the default (includes the [outbound NATS bridge](#outbound-nats-bridge), needs OpenSSL on the target box) and a `-nobridge` variant with no external runtime dependencies.

Pick the latest tag from the releases page and substitute it for `VERSION` below (e.g. `v0.0.18`).

**macOS (Apple Silicon):**

```
VERSION=v0.0.18
curl -LO "https://github.com/lexvicacom/monoblok/releases/download/${VERSION}/monoblok-${VERSION}-macos-aarch64.tar.gz"
tar -xzf "monoblok-${VERSION}-macos-aarch64.tar.gz"
sudo install "monoblok-${VERSION}-macos-aarch64/monoblok" /usr/local/bin/monoblok
brew install openssl@3       # only needed for the bridge build; skip if you grabbed the -nobridge tarball
monoblok --port 4222 --patchbay "monoblok-${VERSION}-macos-aarch64/patchbay.edn"
```

**Linux (x86_64; swap for `linux-aarch64` on ARM):**

```
VERSION=v0.0.18
curl -LO "https://github.com/lexvicacom/monoblok/releases/download/${VERSION}/monoblok-${VERSION}-linux-x86_64.tar.gz"
tar -xzf "monoblok-${VERSION}-linux-x86_64.tar.gz"
sudo install "monoblok-${VERSION}-linux-x86_64/monoblok" /usr/local/bin/monoblok
sudo apt install libssl-dev pkg-config   # only needed for the bridge build; skip if you grabbed the -nobridge tarball
monoblok --port 4222 --patchbay "monoblok-${VERSION}-linux-x86_64/patchbay.edn"
```

Then in another shell, drive it with any NATS client:

```
nats sub 'sensors.*'
(new shell)
nats pub sensors.temp 42.5
```

For more, see next section.
If you don't want OpenSSL on the box at all, grab the `-nobridge` tarball (e.g. `monoblok-${VERSION}-linux-x86_64-nobridge.tar.gz`) and skip the dependency-install step.

### Running as a systemd service on Linux

A unit file and installer ship in [scripts/](./scripts/) and inside the Linux release tarballs (the macOS tarball omits them):

```
sudo bash scripts/install-systemd.sh
sudo systemctl enable --now monoblok
journalctl -u monoblok -f
```

The installer drops the binary at `/usr/local/bin/monoblok`, the patchbay at `/etc/monoblok/patchbay.edn`, creates a `monoblok` system user, and registers the unit. Snapshots live under `/var/lib/monoblok/state.mblk` (created by systemd's `StateDirectory=`, owned by the service user) and are written every 10 seconds, plus once on `systemctl stop`. stdout/stderr land in the systemd journal (`journalctl -u monoblok`), so log rotation, structured fields, and `--since`/`--until` filtering are free.

## Driving the demo patchbay

The shipped [patchbay.edn](./patchbay.edn) wires up a handful of forms on `sensors.*` and `log.app`. Start the server, then in another shell subscribe so you can watch the derived traffic show up:

```
nats sub 'sensors.>'       # in one shell
nats sub 'events.>'        # in another (for the alert rule)
```

1. **`sensors.temp.high`, threshold.** Anything `> 30.0` is mirrored onto `.high`:
   ```
   nats pub sensors.temp 42.5
   ```

2. **`events.alerts`, content match.** Any payload containing `"alert"` is mirrored onto `events.alerts` with the original subject prepended:
   ```
   nats pub log.app "kernel: alert!"
   ```

3. **`sensors.temp.stable`, round + squelch pipeline.** Only emits on a change after rounding to 1 decimal. Send five values; you'll see three `.stable` messages with payloads `42`, `42.1`, `43`:
   ```
   for v in 42.01 42.04 42.08 42.12 43.00; do
       nats pub sensors.temp "$v"
   done
   ```

4. **`sensors.temp.delta`, analog deadband.** Suppresses changes under `0.5`. Send six values; expect three `.delta` emissions (at `10.0`, `10.6`, `11.2`; the others are within deadband of the last accepted anchor):
   ```
   for v in 10.0 10.2 10.4 10.6 10.7 11.2; do
       nats pub sensors.temp "$v"
   done
   ```

5. **`sensors.temp.smoothed`, 10-sample moving average + deadband `1.0`.** Needs a longer stream to drift the mean past each threshold:
   ```
   for v in 10 10 10 10 10 10 10 10 10 10 12 12 12 12 12 14 14 14 14 14; do
       nats pub sensors.temp "$v"
   done
   ```

Stateful ops (`squelch`, `deadband`, `moving-*`) keep their state **per rule, per subject** for the server's lifetime; restart the server to reset. The first sample a rule sees on a subject always passes the gate (no prior value to compare against).

## What is signal conditioning?

Borrowed from electronics, where it means cleaning up a raw analog reading before anything downstream has to deal with it: smoothing noise, ignoring tiny wobbles, snapping to a grid, suppressing duplicates. A temperature sensor that reports 22.031, 22.028, 22.034, 22.031 fifty times a second is technically accurate but annoying to work with; you want "22.0, and tell me when it actually changes."

monoblok does the software version of that, at the broker, before your subscribers ever see the message. `round` snaps to decimals, `quantize` snaps to a step size, `squelch` drops repeats, `deadband` ignores changes below a threshold, `moving-avg` smooths a window. Chain them together and a chatty sensor becomes a clean **"change-only" stream of interesting events**, so subscribers don't have to deal with the noise themselves and can stay simple: **they get a clean signal.**

## Patchbay

patchbay is a small S-expression DSL describing how every incoming publish gets filtered, conditioned, and re-routed. Think of it as a wiring diagram: jacks (subject filters) at the top, filter chains in the middle (`round`, `squelch`, `deadband`, `moving-avg`, ...), and sends (`publish-to`) at the bottom. Top-level forms are `(on SUBJECT-FILTER BODY)`; `BODY` is evaluated whenever an incoming subject matches `SUBJECT-FILTER`. Wildcards are the usual NATS ones: `*` matches one token, `>` matches one-or-more tokens and must be the tail (so `foo.*.*` is exactly three tokens, `foo.>` is three-or-more).

```edn
(on "sensors.*"
  (when (> payload-float 30.0)
    (publish (subject-append "high") payload)))

(on ">"
  (when (contains? payload "alert")
    (publish "events.alerts" (str-concat subject ": " payload))))
```

### Why the electronics vocabulary?

Because the operations already have names, and the names already mean the right thing. `squelch` on a radio suppresses the channel until signal strength changes; here it suppresses the message until the value changes. `deadband` on an industrial controller ignores input movement smaller than a threshold; here it ignores payload changes smaller than a threshold. A "patchbay" in a studio is a grid of jacks you physically wire between sources and destinations, which is exactly what the DSL looks like on the page: subject filters on the left, sends on the right.

Reaching for "filter chain" or "stream transform" works, but they're generic names that carry no intuition: a reader who's seen a hardware squelch knob immediately knows what a `(squelch payload)` call does, whereas "idempotent publish filter" has to be explained from scratch. The terms are also specific enough to resist scope creep, `deadband` is a numeric gate, full stop, so there's no pressure to overload it into something fancier.

It's also just more fun than calling everything `FilterOperatorImpl`.

### What it can be used for

Anywhere you'd otherwise write a small consumer that subscribes, filters or rounds or dedupes, and re-publishes: that consumer becomes a few lines of patchbay, declared once and shared by every downstream subscriber.

Filtering, routing, light payload rewriting, and signal conditioning at the broker. A form inspects an incoming message and can `publish` zero or more derived messages on other subjects; think "threshold this numeric stream onto a `.high` sub-subject," "mirror anything mentioning `alert` into `events.alerts`," "split a firehose into per-tenant subjects," or "deadband a jittery sensor so only meaningful changes hit downstream."

A natural deployment shape is a sidecar at the edge in front of a NATS leaf: noisy publishers (devices, scrapers, log producers) talk to a local monoblok, the patchbay does the rounding / deduping / demuxing / windowing right there, and the [bridge](#outbound-nats-bridge) forwards only the cleaned-up subjects upstream. The real cluster (and its subscribers) sees one tidy stream per concept instead of the raw firehose.

<a id="json-frames"></a>
For sensors that emit JSON frames rather than bare scalars (`{"temp":12.5,"hum":80}`), patchbay has `json-get` for inline lookup of a single field and `json-demux` to break named fields out onto sub-subjects (e.g. `devices.kitchen` `{"temp":12.5,"hum":80}` fans out to `devices.kitchen.temp` and `devices.kitchen.hum`). Top-level keys only, no JSON path; the rest of the patchbay then operates on the resulting scalar streams.

### What it isn't

[NEX](https://github.com/synadia-io/nex) runs arbitrary code (JS, Wasm, binaries) on agents next to the cluster, with HTTP, DB, anything. patchbay is an in-broker DSL with no processes and no sandbox, where the only side effect is `publish`; reach for NEX when you need external I/O.

nats-server's built-in [subject mappings](https://docs.nats.io/nats-concepts/subject_mapping) rewrite subjects with wildcards but can't see the payload or keep state. If all you want is "rename `bar.a.b` to `baz.b.a`," use those; patchbay is for gating on `payload-float` and per-(rule, subject) state (`squelch`, `deadband`, `moving-avg`).

### Patchbay in depth

The full DSL reference (syntax, bound symbols, every operator, all the tables and worked pipelines) lives in [docs/patchbay.md](./docs/patchbay.md).

### Overhead

Per-PUB cost depends on what the matching rule actually does (a cheap `contains?` is nothing like a `moving-avg` over a wide window or a `json-demux` that re-parses the payload), but as a rule of thumb expect ~20-30% throughput off pub-heavy workloads once rules start matching, with fan-out workloads closer to break-even. Cost scales with matching rules per PUB, not total rules in the file. See [`scripts/bench.sh`](./scripts/bench.sh) for the workloads and how to reproduce, and [`--trace`](#--trace-per-evaluation-patchbay-debugger) to see where time is going inside a specific rule.

### Example patchbays

The [`examples/`](./examples/) directory holds runnable patchbay files for common scenarios: [`sensors.edn`](./examples/sensors.edn) (round + squelch on a noisy sensor), [`office-temp.edn`](./examples/office-temp.edn) (deadband + moving-average alert/all-clear), [`ticker.edn`](./examples/ticker.edn) (market data with bridge), [`bars.edn`](./examples/bars.edn) (tick-count OHLC bars per symbol), [`json-frames.edn`](./examples/json-frames.edn) (demux a JSON-emitting device into scalar sub-subjects), [`rental-car.edn`](./examples/rental-car.edn), and [`bridge.edn`](./examples/bridge.edn). Run any of them with `monoblok --patchbay examples/<file>.edn`.

### Patchbay with Claude Code

When getting started writing rules, Claude can help you out. [docs/claude-patchbay.md](./docs/claude-patchbay.md) is a self-contained system prompt that teaches Claude the DSL. Append it to your project's `CLAUDE.md` so Claude Code picks it up automatically when editing `.edn` rule files:

```sh
# project-scoped (recommended)
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/docs/claude-patchbay.md >> ./CLAUDE.md

# or user-global
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/docs/claude-patchbay.md >> ~/.claude/CLAUDE.md
```

For one-off use, reference it inline in a prompt with `@docs/claude-patchbay.md` (Claude Code inlines `@path` refs).

Once it's loaded, describe the stream you have and the stream you want. For example:

> Write a patchbay rule file `ticker.edn` for a noisy market ticker on `MARKET.<SYM>`. Round the price to 3 decimal places and only re-emit when the rounded value changes. Also fan out big jumps to an alerts subject, and bridge those alerts out to a real NATS server at `nats://127.0.0.1:4222`.

<p align="center">
  <img src="claude.png" alt="Claude Code editing a patchbay" width="720">
</p>

Drop `ticker.edn` into a convenient place and run it with `monoblok --patchbay ticker.edn`.


## `$LVC.*`: the last-value-cache stream

Every subject has an implicit last-value cache. Subscribing to `$LVC.foo.bar` joins a live stream of `foo.bar`: current cached value first (if any), then every subsequent publish. Wildcards work. `PUB $LVC.*` is rejected.

```
PUB foo.bar 11      ; cache = 11
PUB foo.bar 12      ; cache = 12
SUB $LVC.foo.bar    ; -> immediately receives 12
PUB foo.bar 13      ; -> subscriber receives 13
```

On by default; `--no-lvc` disables (~2–4% overhead when enabled).

### Snapshots

Warm-start from disk so restarts don't lose the cache or the state inside gates and windows (`deadband`, `squelch`, `moving-avg`, `rising-edge`, etc).

```
monoblok --snapshot /var/lib/monoblok/state.mblk --snapshot-every 10
```

- `--snapshot PATH`: loaded on startup if it exists (missing file is fine, you start empty).
- `--snapshot-every SECONDS`: periodic background dump (atomic temp-file + rename, runs on a worker thread so the event loop stays responsive). Omit for load-only.
- `SIGINT` / `SIGTERM` always writes a final snapshot before exiting.

If the patchbay file changes between runs, LVC entries still load; rule state for any rule whose filter no longer matches at its recorded position is skipped with a warning.

## `$STATS.*`: live counters

The server publishes cumulative counters to `$STATS.*` on a 1-minute wall-clock tick. Values are u64 decimals; subscribers compute their own rates across ticks.

| subject                          | value                                             |
|----------------------------------|---------------------------------------------------|
| `$STATS.global.pubs`             | total inbound client PUBs since server start      |
| `$STATS.rules.<i>.emitted`       | successful `publish` / `publish-to` calls by rule |
| `$STATS.rules.<i>.suppressed`    | gate suppressions (squelch, deadband, changed?, rising-edge, falling-edge) by rule |
| `$STATS.bridge.published`        | publishes forwarded to the remote NATS cluster (if the bridge is configured) |
| `$STATS.bridge.dropped`          | publishes the bridge failed to forward (connection closed, subject too long, remote error) |

Rules are indexed by position in the patchbay file, 0-based. Client publishes to `$STATS.*` are rejected (read-only, like `$LVC.*`). The stream is deliberately **not** cached by the LVC — a tick-driven snapshot is stale the moment the next tick fires — so `SUB $STATS.>` for live deltas is the only subscription that makes sense.

## Outbound NATS bridge

<p align="center">
  <img src="bridge.png" alt="bridge" width="720">
</p>

monoblok can forward a subset of local publishes to a real NATS cluster, so it can sit in front of (or alongside) a NATS deployment and hand off selected traffic. **Export-only**: nothing flows in from the remote. TLS and `.creds` files are supported; the upstream connection uses vendored [nats.c](https://github.com/nats-io/nats.c) and dyn-linked OpenSSL.

Typical shape:

- Local clients publish to short, cheap subjects on monoblok.
- Patchbay rules condition the signal (round, deadband, squelch, moving averages, etc.).
- The bridge forwards only the subjects you want (raw sensor reads, the derived stable ones, or both) to a remote NATS cluster — Synadia NGS, a managed cluster, a self-hosted cluster, etc.
- Nothing else leaves the box. Everything off-path stays local.

Using monoblok as a front-NATS for existing publishers is an elegant addition: what a downstream consumer used to do (rounding, deadbanding, squelching) can move into monoblok, and everyone subscribing to the processed subjects is none the wiser, they just connect to the same production NATS environment. NATS isn't strictly required downstream either: in smaller or more experimental setups, consumers can point straight at monoblok.

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
| `:export`                   | list of strings | subject filters. Same `*` / `>` semantics as `(on ...)` and NATS `SUB`: `*` is one token, `>` is tail-only and matches one-or-more. Local publishes matching any filter are forwarded. |
| `:name`                     | string          | client name shown in the remote's monitoring      |
| `:creds`                    | path            | JWT + NKey credentials file (NGS / operator mode) |
| `:user` / `:password`       | string          | basic auth                                        |
| `:token`                    | string          | bearer-token auth                                 |
| `:tls`                      | bool            | enable TLS. Required if any `:servers` URL is `tls://`. |
| `:connect-timeout-ms`       | number          | initial-connect timeout                           |
| `:ping-interval-ms`         | number          | keepalive ping cadence                            |
| `:max-reconnect`            | number          | `-1` for unlimited                                |
| `:reconnect-wait-ms`        | number          | base delay between reconnect attempts             |

Auth precedence: `:creds` > `:user`/`:password` > `:token`. If TLS is on but `:tls-ca` isn't set, nats.c falls back to the system trust store.

### Semantics

A local publish (from a NATS client or from a patchbay rule) whose subject matches **any** `:export` filter is forwarded to the remote as-is. Subjects that don't match any filter never leave the server. Fan-out order is: local subscribers served first, bridge second — so a slow or reconnecting remote can't starve local delivery.

Reconnects are handled by nats.c internally. During the reconnect window, publishes are buffered up to the library default; once the buffer is full, further publishes count as dropped. Counters are published on the `$STATS.*` tick as `$STATS.bridge.published` and `$STATS.bridge.dropped`.

### Disabling the bridge

The bridge is on by default. Turn it off at build time with `zig build -Dbridge=false`, which also removes the OpenSSL link dependency. Both variants are published to each release as `-nobridge` and (default) archives — pick whichever fits the target box. Or simply do not add a `bridge` form to your config if you do not need it.


## Observability

Every accepted and closed connection logs a line at info level (`conn 42 accepted` / `conn 42 closed`), keyed by the same connection id used in the warn lines below. Most NATS clients hold one long-lived TCP connection, so this is normally O(clients) not O(messages).

Two always-on warn thresholds fire if something's off:

- **Patchbay amplification**: a single inbound PUB that causes 64+ rule-generated publishes logs once with the offending subject. Catches runaway fan-out from a misconfigured rule.
- **Outbound buffer high-water**: if any connection's pending-write buffer grows past 4 MiB between drains, logs the conn id and the size. Usually means a slow consumer or a fan-out target that stopped reading.

`--stats` opts into a periodic summary line every 10k inbound PUBs, with the observed max rule-publishes-per-input and max per-conn outbound buffer over the window:

```
info: stats: pubs=10000 max_rule_publishes=0 max_out_hwm=41160B
```

Useful for seeing how much headroom you have under the warn thresholds. No output when `--stats` is off.

### `--trace`: per-evaluation patchbay debugger

`--trace` prints every patchbay form the evaluator visits to stderr, with the form's result and the elapsed time spent inside it. One line per inbound PUB lists the subject and payload; each matching rule prints its filter, then the body is unrolled with one indented line per call, and a `=> result [duration]` line per call afterwards. Per-rule and per-form timings include nested calls. After all rules run, a `total [duration]` line shows the wall time across the whole PUB.

Side-effecting ops (`publish`, `publish-to`, `count`, `json-demux`, `ohlc-bar`) all return `nil`, so the trace replaces the bare `=> nil` at the leaf with `=> published "subj" payload [duration]` so you can tell "did the thing" from "was suppressed". Forms that returned `nil` because a gate stopped the flow get a parenthetical hint: `=> nil (squelched)`, `=> nil (within deadband)`, `=> nil (no rising edge)`, `=> nil (branch not taken)`, etc. The hint is a static gloss on the head symbol (it doesn't know the precise reason — `rising-edge` shows the same hint on first sight and on stayed-false), but it's enough to disambiguate suppression from successful side-effects in most cases.

```
$ monoblok --port 4222 --patchbay patchbay.edn --trace
warning: --trace enabled: every patchbay evaluation will be printed to stderr (loud, do not run in production)
...
trace: sensors.temp 42.5
  rule 0 (on "sensors.*") matched
  (when (> payload-float 30) (publish (subject-append "high") payload))
    (> payload-float 30)
      => true [124µs]
    (publish (subject-append "high") payload)
      (subject-append "high")
        => "sensors.temp.high" [113µs]
      => published "sensors.temp.high" 42.5 [549µs]
    => nil [921µs]
  rule 0 done [1ms]
  rule 2 (on "sensors.*") matched
  (-> payload-float (round 1) (squelch) (publish-to (subject-append "stable")))
    (round 1 42.5)
      => 42.5 [2µs]
    (squelch 42.5)
      => nil (squelched) [237µs]
    ...
total [3ms]
```

Use it to figure out why a rule isn't firing (gate suppressed, predicate false), or to spot which form in a `->` pipeline is dominating evaluation time. The evaluator picks a traced or non-traced path once per `run`, so the flag-off cost is zero (no per-node branching).

Loud by design (every PUB prints), so this is a debugging tool, not a production observability surface. Pipe stderr to a file (`monoblok --trace 2>trace.log`) when investigating.

Tracing also adds per-form overhead (two `clock_gettime` calls and a stderr write at every node, plus the leaf-emission bookkeeping), so the timings you see under `--trace` are inflated relative to a normal run. Don't use traced timings for benchmarking, and don't read a 1ms PUB under `--trace` as 1ms in production — bench with `--release=fast` and the flag off.

## Architecture

One `xev.Loop` owns accept, per-connection read/write completions, router state, and the LVC. No mutexes, no atomics on the hot path. Fan-out appends bytes directly to each subscriber's outbound `ArrayList` and kicks a single `write` per connection per publish, with partial-write handling.

### Single-threaded, on purpose

Everything application-level runs on a single thread: parsing, subject matching, rule evaluation, fan-out, write buffering. The kernel still gets to use your other cores for actual I/O, but once a byte arrives it's single file through monoblok on a single thread.

monoblok is designed for one core because that's the right shape for the workload. Signal conditioning is cheap per message, the patchbay state lives in a single address space with no synchronization, and an event loop without locks is straightforwardly fast and straightforwardly debuggable. Adding a second thread would mean reaching for atomics or mutexes on every shared structure (router table, LVC, per-rule state), and the resulting code would be slower in the common case and harder to reason about in every case.

The cap is one core's worth of throughput per instance, and the interesting question is how far that actually gets you. The benchmark table further down shows millions of msgs/sec on a single core for the workloads monoblok is built for; a signal-conditioning patchbay sitting in front of sensor or telemetry traffic is nowhere near that ceiling. Size the patchbay to the box and you have a lot of headroom on hardware that costs a few quid a month.

### Deploying

Pick a 2-vCPU VM with 256 MB of RAM. monoblok runs on one core, the kernel net stack and io_uring workers use the other; a 1-vCPU box makes them fight over the same core (~1.6× slowdown, see [Benchmarks](#benchmarks)). More than two cores is wasted spend (the extras sit idle). Builds ship for `linux-aarch64` and `linux-x86_64`.

Concretely: Hetzner CAX11 (2 vCPU Ampere Altra, ~€4/mo) is the sweet spot, AWS `t4g.small` or `c7g.large` / `c8g.large` if you want Graviton, Oracle Ampere A1 free tier for kicking the tyres (host-oversubscribed in practice, so don't lean on it for throughput). The CAX11 sustains ~2.4M msgs/sec PUB and ~2.1M msgs/sec on a 10-subscriber fan-out with the demo patchbay loaded.

The systemd unit in [scripts/](./scripts/) plus `--snapshot` handles restarts: the unit restarts on failure, the snapshot reloads LVC values and gate/window state on startup, so a crash or reboot loses at most one snapshot interval (10 s by default) of in-flight conditioning state. Subscribers reconnect automatically.

If you're bridging to an upstream NATS cluster, that cluster is the system of record. A cheap VM dying loses you smoothing for the duration of the outage, not data: anything already exported is durable upstream, and the snapshot restores the gates on restart so they don't re-fire on stale comparisons.

### Why libxev

Zig 0.16's `std.Io` works, but not in a way that fits a single-threaded event loop: the macOS `Dispatch` backend is thread-per-connection and didn't always compile cleanly for me on 0.16 (could well be me holding it wrong; or maybe it is too low level for my small brain). libxev worked first try, and gives us a proper single-loop model on kqueue / io_uring / epoll / IOCP, picked at comptime. The server logs which backend it's using at startup.

## Tests

```
zig build test              # unit tests
bash scripts/smoke.sh       # end-to-end over raw TCP
bash scripts/bench.sh       # pub + fan-out bench (needs `nats` CLI)
```

## Benchmarks

`nats-server` is a mature Go codebase doing a lot more than monoblok (accounting, metrics, slow-consumer detection, clustering, JetStream, TLS, auth). These numbers are informational, not a "faster than nats-server" claim. monoblok is benchmarked with an **empty patchbay**, so this is raw PUB/SUB + fan-out only; a real patchbay adds work per matching publish.

Both columns are msgs/sec from `nats bench`, single run each. monoblok built `--release=safe`, vs `nats-server` v2.12.7.

`scripts/bench.sh` drives the numbers: it starts monoblok on `$NATS_URL` (default `127.0.0.1:4222`), runs the six `nats bench` workloads in the table below against it, stops it, then (if `nats-server` is on `PATH`) starts it on the same port and reruns the same workloads. Servers are benched sequentially, never concurrently. Pub workloads use `nats bench pub`, fan-out workloads spawn a `nats bench sub` with N clients and then a single publisher on the same subject. The script scrapes the `publisher stats` / `subscriber stats` line from each run and prints the msgs/sec table at the end.

**M4 Mac Mini** (10-core, 16 GB, macOS 26.2, kqueue) vs **Hetzner CAX11** (2-vCPU Ampere Altra Neoverse-N1, 4 GB, Ubuntu 24.04, io_uring):

| workload            |    M4 monoblok |     M4 nats |   M4 Δ | CAX11 monoblok | CAX11 nats | CAX11 Δ |
|---------------------|---------------:|------------:|-------:|---------------:|-----------:|--------:|
| 1 pub × 500k × 64B  |       6.46M/s  |    7.14M/s  |    −9% |       2.35M/s  |   2.07M/s  |    +14% |
| 2 pub × 10k × 64B   |       7.35M/s  |    5.97M/s  |   +23% |       1.50M/s  |   2.45M/s  |    −39% |
| 8 pub × 50k × 128B  |      11.83M/s  |    8.17M/s  |   +45% |       1.96M/s  |   2.05M/s  |     −5% |
| 1 pub → 1 sub       |       4.27M/s  |    3.93M/s  |    +9% |       0.62M/s  |   0.94M/s  |    −34% |
| 1 pub → 10 subs     |      12.60M/s  |    4.95M/s  |  +155% |       2.11M/s  |   1.83M/s  |    +15% |
| 1 pub → 50 subs     |      17.52M/s  |    4.82M/s  |  +264% |       2.38M/s  |   1.86M/s  |    +28% |

Fan-out is where monoblok pulls ahead on both platforms (the 1-sub workload is the standing exception, likely a low-concurrency bug). Multi-publisher wins on the M4 narrow on the CAX11, since a single-threaded loop can't scale past one core while nats-server spreads across both vCPUs. The Ampere Altra column is the more honest deployment-shape number, a single ARM core on a cheap VM, which is roughly what a real monoblok install looks like, and even there a single-threaded Zig loop holds its own against the multi-threaded Go server on most workloads. `--release=fast` adds ~10–15% on top. Take this all with a pinch of salt. **NATS is still the reliable, tuned Porsche and monoblok is a rusty Civic with a bolted-on eBay turbo :)**

### Single core, but you still want at least two

monoblok itself runs on a single core, so you might think a 1-vCPU box is the right shape. It mostly isn't. The kernel's network stack, io_uring's worker threads, and the bench client (or in production, whatever's connected over loopback) all want CPU too, and on a 1-vCPU box they timeshare with monoblok. A second vCPU lets the broker run flat-out on core 0 while everything-else-on-the-box uses core 1.

Same Neoverse-N1 silicon, same patchbay, same workloads, run on Hetzner CAX11 (2 vCPU, ~€4/mo) vs Oracle A1 free tier (1 OCPU):

| workload                |    CAX11 (2-core) | Oracle free (1-core) | speedup |
|-------------------------|------------------:|---------------------:|--------:|
| 1 pub × 1M × 64B        |          2.46M/s  |             1.53M/s  |   1.6×  |
| 2 pub × 500k × 64B      |          3.01M/s  |             1.66M/s  |   1.8×  |
| 8 pub × 200k × 128B     |          2.21M/s  |             1.45M/s  |   1.5×  |
| 1 pub → 1 sub           |          0.59M/s  |             0.45M/s  |   1.3×  |
| 1 pub → 10 subs         |          2.11M/s  |             1.17M/s  |   1.8×  |
| 1 pub → 50 subs         |          2.37M/s  |             1.33M/s  |   1.8×  |

Both boxes measure the same effective clock (~2.95 GHz) and ~0% steal, so it isn't clock and it isn't oversubscription, it's just that "1 vCPU" really does mean monoblok and the kernel net stack fight over the same core. The takeaway: pick the cheapest 2-vCPU plan your provider sells, not the cheapest 1-vCPU one.

### Patchbay overhead

Empty patchbay vs 1 rule vs 50 rules on the CAX11 (`bash scripts/bench.sh`):

| workload                |    no patchbay |       1 rule | 50 rules |
|-------------------------|---------------:|-------------:|---------:|
| 1 pub × 1M × 64B        |       2.46M/s  |     2.40M/s  | 2.51M/s  |
| 2 pub × 500k × 64B      |       3.01M/s  |     2.24M/s  | 2.22M/s  |
| 8 pub × 200k × 128B     |       2.21M/s  |     1.81M/s  | 2.07M/s  |
| 1 pub → 1 sub           |       0.59M/s  |     0.68M/s  | 0.63M/s  |
| 1 pub → 10 subs         |       2.11M/s  |     2.07M/s  | 2.09M/s  |
| 1 pub → 50 subs         |       2.37M/s  |     2.43M/s  | 2.40M/s  |

The cost scales with **matching rules per PUB**, not total rules in the file: 1 rule and 50 rules land in roughly the same place because the dispatch table only invokes the rules whose subject filter actually matches. The 2-publisher row is the worst case (~25% off) where every PUB matches a rule; fan-out workloads are at break-even because the bottleneck is the write side, not the rule. Real patchbays sit somewhere in between depending on what the rules actually do (a `contains?` is nothing like a `moving-avg` over a wide window).

## Building from source

Zig 0.16.0. OpenSSL at runtime is required when the bridge is enabled (the default); skip it if you build with `-Dbridge=false`.

```
# macOS
brew install openssl@3

# Debian / Ubuntu
sudo apt install libssl-dev pkg-config
```

Then:

```
zig build --release=safe
./zig-out/bin/monoblok --port 4222 --patchbay patchbay.edn
```

Release binaries are built natively on each target architecture, not cross-compiled. The `.github/workflows/release.yml` pipeline uses three runners: `ubuntu-22.04` (x86_64), `ubuntu-22.04-arm` (aarch64), and `macos-latest` (aarch64). Each runs `zig build --release=safe` assuming OpenSSL is installed locally.

Each release ships **two variants per platform**: with the NATS bridge (requires OpenSSL on the target box) and without (`-nobridge` suffix, no runtime deps). Grab the bridge variant if you want to forward traffic to a real NATS cluster; grab the `-nobridge` variant if you just want a standalone pub/sub broker with no external deps.

## AI
Yes, Claude helps. [Some thoughts on this](https://github.com/lexvicacom/monoblok/blob/main/docs/how-monoblok-uses-ai.md)

## License

MIT. See `LICENSE`.
