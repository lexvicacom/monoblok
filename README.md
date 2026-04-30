<p align="center">
  <img src="monoblok.png" alt="monoblok" width="480" style="border-radius: 16px; box-shadow: 0 8px 24px rgba(0,0,0,0.25);">
</p>

# monoblok

monoblok is a one-file declarative stream-processor with a NATS-shaped front and back. Publishers PUB to it like any NATS server; a small S-expression DSL called **patchbay** rounds, deduplicates, deadbands, smooths, demuxes JSON, builds OHLC bars; subscribers (or a real upstream cluster, via the bridge) get the cleaned stream. The cleanup is declared once instead of being re-implemented in every consumer.

It's a single small binary written in Zig, statically self-contained beyond libc. Last-value streams on `$LVC.*` give late subscribers the current value per subject. Useful for jittery sensors (the £2.99 Temu kind), high-frequency market data, anything where most of the data movement isn't worth a downstream message. [Read the introductory blog post](https://alexjreid.dev/posts/monoblok/).

## Try it out with no install

A [public demo server](https://alexjreid.dev/posts/monoblok-demo/) runs on `nats://monoblok.rtd.pub:4222`, with a bridged real NATS server on `nats://monoblok.rtd.pub:4223`. Point any `nats` CLI at the first and start publishing. See [docs/demo.md](./docs/demo.md) for the loaded patchbay and subjects worth subscribing to.

## Install

Prebuilt Mac (Apple Silicon) and Linux (x86_64, aarch64) binaries on the [latest release page](https://github.com/lexvicacom/monoblok/releases/latest). Pick the latest tag and substitute it for `VERSION`:

```
VERSION=v0.0.26
PLATFORM=macos-aarch64   # or linux-x86_64, linux-aarch64
curl -LO "https://github.com/lexvicacom/monoblok/releases/download/${VERSION}/monoblok-${VERSION}-${PLATFORM}.tar.gz"
tar -xzf "monoblok-${VERSION}-${PLATFORM}.tar.gz"
sudo install "monoblok-${VERSION}-${PLATFORM}/monoblok" /usr/local/bin/monoblok
monoblok --port 4222 --patchbay "monoblok-${VERSION}-${PLATFORM}/patchbay.edn"
```

Then drive it with any NATS client:

```
nats sub 'sensors.*'
nats pub sensors.temp 42.5
```

### systemd

Linux release tarballs ship a unit file and installer in [scripts/](./scripts/):

```
sudo bash scripts/install-systemd.sh
sudo systemctl enable --now monoblok
journalctl -u monoblok -f
```

Drops the binary at `/usr/local/bin/monoblok`, the patchbay at `/etc/monoblok/patchbay.edn`, snapshots under `/var/lib/monoblok/state.mblk` (every 10s plus on stop), and creates a `monoblok` system user.

### Deploying

A 2-vCPU VM with 256 MB+ of RAM is the right shape. monoblok runs on one core; the kernel net stack and io_uring workers will use the other. A 1-vCPU box makes them share the same core, costing throughput. More than two cores is wasted spend (the extras sit idle). Hetzner CAX11 (about €5/mo) is the sweet spot; AWS `t4g.small` or `c7g.large` works for Graviton; Oracle Ampere A1 free tier for kicking the tyres.

The systemd unit plus `--snapshot` handles restarts: a crash or reboot loses at most one snapshot interval of in-flight conditioning state. If you're bridging upstream, that cluster is the system of record (anything already exported is durable there).

## Patchbay

patchbay is a small S-expression DSL describing how every incoming publish gets filtered, conditioned, and re-routed. Top-level forms are `(on SUBJECT-FILTER BODY)`; `BODY` is evaluated whenever an incoming subject matches `SUBJECT-FILTER`. Wildcards are NATS-style: `*` is one token, `>` is the tail.

```edn
(on "sensors.*"
  (when (> payload-float 30.0)
    (publish (subject-append "high") payload)))

; Round to 1dp, drop duplicates, emit on the .stable sub-subject.
(on "sensors.*"
  (-> payload-float
      (round 1)
      (squelch)
      (publish-to (subject-append "stable"))))
```

The vocabulary is borrowed from electronics (`squelch` suppresses until the value changes, `deadband` ignores movement smaller than a threshold) because the names already mean the right thing. A "patchbay" in a studio is a grid of jacks you wire between sources and destinations, which is exactly what the DSL looks like on the page.

JSON frames like `{"temp":12.5,"hum":80}` can be demuxed onto scalar sub-subjects (`json-demux`) and conditioned the same way; top-level keys only.

Full reference and worked examples in [docs/patchbay.md](./docs/patchbay.md). One-line summary of every form in the [cheatsheet](./docs/patchbay-cheatsheet.md). Runnable end-to-end demos in [`examples/`](./examples/); each `.edn` has a matching `.sh` that starts monoblok, publishes a sequence, subscribes in parallel, and prints publishes vs deliveries.

| file                                              | what it shows                                                   |
|---------------------------------------------------|-----------------------------------------------------------------|
| [`sensors.edn`](./examples/sensors.edn)           | round + squelch on a noisy sensor                               |
| [`office-temp.edn`](./examples/office-temp.edn)   | moving-average alert + all-clear via `transition` and `count`   |
| [`ticker.edn`](./examples/ticker.edn)             | market data: round, squelch, big-jump alerts, bridge            |
| [`bars.edn`](./examples/bars.edn)                 | tick-count OHLC bars per symbol                                 |
| [`latency-stats.edn`](./examples/latency-stats.edn) | live p50/p95/p99/stddev over a sliding window                 |
| [`json-frames.edn`](./examples/json-frames.edn)   | `json-demux` a JSON-emitting device into scalar sub-subjects    |
| [`rental-car.edn`](./examples/rental-car.edn)     | quantize + deadband + over-rev hold-off alert                   |
| [`bridge.edn`](./examples/bridge.edn)             | forward selected subjects to a real NATS server                 |
| [`demo.edn`](./examples/demo.edn)                 | tour of every primitive on `demo.sensors.*`                     |
| [`lvc.edn`](./examples/lvc.edn)                   | `$LVC.>` cache replay: a late joiner gets the last value        |

Run a patchbay directly with `monoblok examples/<file>.edn`; form-lint without starting the server with `monoblok --validate examples/<file>.edn`.

### What it isn't

[NEX](https://github.com/synadia-io/nex) runs arbitrary code (JS, Wasm, binaries) on agents next to the cluster, with HTTP, DB, anything. patchbay is an in-broker DSL with no processes and no sandbox where the only side effect is `publish`; reach for NEX when you need external I/O. nats-server's built-in [subject mappings](https://docs.nats.io/nats-concepts/subject_mapping) rewrite subjects with wildcards but can't see the payload or keep state; use those if all you want is "rename `bar.a.b` to `baz.b.a`".

### Patchbay overhead

Per-PUB cost depends on what the matching rule actually does. Rule of thumb: ~20–30% off pub-heavy throughput once rules start matching, fan-out workloads closer to break-even. Cost scales with **matching** rules per PUB, not total rules in the file. See [Benchmarks](#benchmarks) below and [`--trace`](#--trace-per-evaluation-debugger) to see where time goes inside a specific rule.

### Patchbay with Claude Code

[docs/claude-patchbay.md](./docs/claude-patchbay.md) is a self-contained system prompt that teaches Claude the DSL. Append it to your project's `CLAUDE.md` so Claude Code picks it up automatically when editing `.edn` rule files:

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/docs/claude-patchbay.md >> ./CLAUDE.md
```

Once loaded, describe the stream you have and the stream you want.

<p align="center">
  <img src="claude.png" alt="Claude Code editing a patchbay" width="720">
</p>

## `$LVC.*`: last-value-cache stream

Every subject has an implicit last-value cache. Subscribing to `$LVC.foo.bar` joins a live stream of `foo.bar`: current cached value first (if any), then every subsequent publish. Wildcards work. `PUB $LVC.*` is rejected.

```
PUB foo.bar 11      ; cache = 11
PUB foo.bar 12      ; cache = 12
SUB $LVC.foo.bar    ; -> immediately receives 12
PUB foo.bar 13      ; -> subscriber receives 13
```

On by default; `--no-lvc` disables (~2–4% overhead when enabled).

### Snapshots

Warm-start from disk so restarts don't lose the cache or the state inside gates and windows.

```
monoblok --snapshot /var/lib/monoblok/state.mblk --snapshot-every 10
```

`--snapshot PATH` loads on startup if it exists (missing is fine). `--snapshot-every SECONDS` runs a periodic background dump (atomic temp-file + rename, on a worker thread). `SIGINT` / `SIGTERM` always writes a final snapshot before exiting. If the patchbay file changes between runs, LVC entries still load; rule state for any rule whose filter no longer matches at its recorded position is skipped with a warning.

## `$STATS.*`: live counters

The server publishes cumulative counters to `$STATS.*` on a 1-minute wall-clock tick. Values are u64 decimals; subscribers compute their own rates across ticks.

| subject                          | value                                             |
|----------------------------------|---------------------------------------------------|
| `$STATS.global.pubs`             | total inbound client PUBs since start             |
| `$STATS.rules.<i>.emitted`       | successful `publish` / `publish-to` calls by rule |
| `$STATS.rules.<i>.suppressed`    | gate suppressions by rule                         |
| `$STATS.bridge.published`        | publishes forwarded to the remote NATS cluster    |
| `$STATS.bridge.dropped`          | publishes the bridge failed to forward            |

Rules are indexed by position in the patchbay file, 0-based. Client publishes to `$STATS.*` are rejected. The stream is deliberately **not** cached by the LVC.

## Outbound NATS bridge

<p align="center">
  <img src="bridge.png" alt="bridge" width="720">
</p>

monoblok can forward a subset of local publishes to a real NATS cluster. **Export-only**: nothing flows in from the remote. TLS and `.creds` files are supported via vendored [nats.zig](https://github.com/nats-io/nats.zig).

Zero or one `(bridge ...)` form in the patchbay file configures it:

```edn
(bridge
  :servers  ("tls://connect.ngs.global:4222")
  :creds    "/etc/monoblok/ngs.creds"
  :tls      true
  :name     "monoblok-prod-1"
  :export   ("telemetry.>" "alerts.>"))
```

A local publish (from a NATS client or a patchbay rule) whose subject matches any `:export` filter is forwarded as-is. Local subscribers are served first, bridge second, so a slow remote can't starve local delivery. Reconnects are handled inside nats.zig.

Full keyword reference (auth, timeouts, reconnect tuning) in [docs/patchbay-cheatsheet.md](./docs/patchbay-cheatsheet.md).

## Listeners

By default monoblok listens on TCP (`--port`, default 4222). It can additionally or instead listen on an AF_UNIX stream socket:

```
monoblok --port 4222 --unix-socket /tmp/monoblok.sock --patchbay patchbay.edn   # both
monoblok --port 0    --unix-socket /tmp/monoblok.sock --patchbay patchbay.edn   # unix only
```

Both listeners share the same router, so a publish from one side fans out to subscribers on the other. The socket is mode 0600, removed on graceful shutdown, stale files unlinked on startup.

## Observability

Every accepted/closed connection logs at info level. Two always-on warn thresholds fire if something's off:

- **Patchbay amplification**: a single inbound PUB causing 64+ rule-generated publishes logs once with the offending subject.
- **Outbound buffer high-water**: a connection's pending-write buffer growing past 4 MiB between drains logs the conn id and size (slow consumer or fan-out blow-up).

`--stats` prints a periodic summary every 10k inbound PUBs:

```
info: stats: pubs=10000 max_rule_publishes=0 max_out_hwm=41160B
```

### `--trace`: per-evaluation debugger

Prints every patchbay form the evaluator visits to stderr, with result and elapsed time. Side-effecting ops show `=> published "subj" payload [duration]` instead of bare nil; suppressed gates show a hint (`=> nil (squelched)`, `=> nil (within deadband)`, etc).

```
$ monoblok --port 4222 --patchbay patchbay.edn --trace
trace: sensors.temp 42.5
  rule 0 (on "sensors.*") matched
  (when (> payload-float 30) (publish (subject-append "high") payload))
    (> payload-float 30)
      => true [124µs]
    (publish (subject-append "high") payload)
      => published "sensors.temp.high" 42.5 [549µs]
total [3ms]
```

Loud by design (every PUB prints), so pipe stderr to a file when investigating. Adds per-form overhead, so the timings under `--trace` are inflated relative to a normal run; bench with `--release=fast` and the flag off.

## Architecture

One `xev.Loop` owns accept, per-connection read/write completions, router state, and the LVC. No mutexes, no atomics on the hot path. Fan-out appends bytes directly to each subscriber's outbound buffer and kicks a single `write` per connection per publish, with partial-write handling.

Everything application-level runs on a single thread: parsing, subject matching, rule evaluation, fan-out, write buffering. The kernel still uses your other cores for I/O, but once a byte arrives it's single-file through monoblok. Adding a second thread would mean atomics or locks on every shared structure (router, LVC, per-rule state) and would be slower in the common case. The cap is one core's worth of throughput per instance, and the benchmarks below show that's a lot of headroom for signal conditioning workloads.

Zig 0.16's `std.Io` networking didn't fit a single-loop model on 0.16 (the macOS Dispatch backend is thread-per-connection), so the loop is libxev: proper kqueue / io_uring / epoll / IOCP picked at comptime. The server logs which backend it's using at startup.

## Tests

```
zig build test              # unit tests
bash scripts/smoke.sh       # end-to-end over raw TCP
bash scripts/bench.sh       # pub + fan-out bench (needs `nats` CLI)
```

## Benchmarks

FYI rather than scientific. `nats-server` is a mature Go codebase doing a lot more than monoblok (accounting, metrics, slow-consumer detection, clustering, JetStream, TLS, auth). These numbers are not a "faster than nats-server" claim. monoblok is benchmarked with an **empty patchbay**, so this is raw PUB/SUB + fan-out only; a real patchbay adds work per matching publish.

Numbers are msgs/sec from `nats bench`, single run each. monoblok built `--release=safe`, vs `nats-server` v2.12.7. `scripts/bench-with-nats-server.sh` drives the table sequentially with a 5s cooldown between rows so back-to-back runs don't thermally throttle the host.

**MacBook Air M2** (8-core, 16 GB, macOS 15.6, kqueue):

| workload            |   monoblok |  nats-server |     Δ |
|---------------------|-----------:|-------------:|------:|
| 1 pub × 500k × 64B  |    8.52M/s |      8.91M/s |   −4% |
| 2 pub × 10k × 64B   |    4.60M/s |      3.90M/s |  +18% |
| 8 pub × 50k × 128B  |    5.32M/s |      4.47M/s |  +19% |
| 1 pub → 1 sub       |    3.00M/s |      3.16M/s |   −5% |
| 1 pub → 10 subs     |    8.67M/s |      4.03M/s | +115% |
| 1 pub → 50 subs     |   11.18M/s |      2.97M/s | +276% |

Fan-out is where monoblok pulls ahead hardest, but multi-publisher and pub-only rows hold their own. The 1-sub row is the standing exception (likely a low-concurrency bug). `--release=fast` adds another ~10–15%. Take this all with a pinch of salt: NATS is the reliable, tuned Porsche; monoblok is a rusty Civic with a bolted-on eBay turbo.

### Patchbay overhead

Empty patchbay vs 1 rule vs 50 rules on the same M2 (`bash scripts/bench.sh`):

| workload                |    no patchbay |       1 rule |    Δ |     50 rules |    Δ |
|-------------------------|---------------:|-------------:|-----:|-------------:|-----:|
| 1 pub × 1M × 64B        |        9.45M/s |      7.14M/s | −24% |      6.48M/s | −31% |
| 2 pub × 500k × 64B      |        8.65M/s |      6.62M/s | −23% |      6.62M/s | −24% |
| 8 pub × 200k × 128B     |        6.54M/s |      5.10M/s | −22% |      5.21M/s | −20% |
| 1 pub → 1 sub           |        3.41M/s |      3.37M/s |  −1% |      3.03M/s | −11% |
| 1 pub → 10 subs         |        8.13M/s |      8.71M/s |  +7% |      8.52M/s |  +5% |
| 1 pub → 50 subs         |       11.62M/s |     10.55M/s |  −9% |     10.59M/s |  −9% |

Cost scales with matching rules per PUB, not total rules in the file: 1 rule and 50 rules land in roughly the same place because the dispatch table only invokes rules whose filter actually matches.

## Building from source

Zig 0.16.0 exactly. No system libraries (the bridge uses vendored [nats.zig](https://github.com/nats-io/nats.zig), pure-Zig with `std.crypto.tls`).

```
zig build --release=safe
./zig-out/bin/monoblok --port 4222 --patchbay patchbay.edn
```

Release binaries are built natively per arch by `.github/workflows/release.yml` on three runners: `ubuntu-22.04` (x86_64), `ubuntu-22.04-arm` (aarch64), `macos-latest` (aarch64).

## AI

Yes, Claude helps. [Some thoughts on this](https://github.com/lexvicacom/monoblok/blob/main/docs/how-monoblok-uses-ai.md).

## License

MIT. See `LICENSE`.
