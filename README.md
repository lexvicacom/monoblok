# monoblok

Put monoblok in front of NATS when the raw stream is too noisy.

monoblok is a tiny NATS-compatible stream conditioner: publishers send raw events, **patchbay** rules clean and reshape them once, and subscribers receive stable derived streams. It speaks enough of the [NATS](https://nats.io) protocol to be useful: publishers PUB to it like any NATS server; a small S-expression DSL rounds, deduplicates, deadbands, smooths, demuxes JSON, and builds OHLC bars; subscribers (or a real upstream NATS cluster, via the bridge) get the cleaned stream. The _conditioning_ is declared once, instead of being re-implemented by every consumer.

![monoblok round and squelch demo](./docs/monoblok-round-squelch-fixed.gif)

>The suggested topology is to run monoblok as a _conditioning twig_: it cleans and shapes local streams, then exports the results directly to a NATS cluster, or through a real NATS leaf, via the bridge. [tinyblok](https://github.com/lexvicacom/tinyblok) can sit even further out, running the same patchbay DSL on ESP32 chips. **Conditioning at the edge, analysis in the cloud.**

For smaller scenarios, NATS is optional: every monoblok process is a NATS core broker, so you can run one directly in the cloud and point clients straight at it. Absolutely reach for real NATS when you want clustering, JetStream, or a long-lived system of record; monoblok stays focused on conditioning the stream.

Use it anywhere the _same old processing/cleaning code_ gets monotonous to write across consumers: cheap sensors with noisy readings, market data feeds, telemetry. **It reduces downstream work.** [Read the introductory blog post](https://alexjreid.dev/posts/monoblok/) [and friends](https://alexjreid.dev/tags/monoblok/).

monoblok can help if:

- you have noisy numeric streams (sensors, tickers, telemetry) and every consumer is reimplementing the same round/dedupe/deadband/smooth code
- you want a small box at the edge that cleans data, saving paying to ship the noise
- you want last-value replay (`$LVC.*`) so a late subscriber sees current state immediately, without standing up a separate cache
- you want OHLC bars, moving stats, or windowed aggregates declared once in a config file rather than coded per consumer

## Try it out with no install

A [public demo server](https://alexjreid.dev/posts/monoblok-demo/) runs on `nats://demo.monoblok.host:4222`, with a bridged real NATS server on `nats://demo.monoblok.host:4223`. Point any `nats` CLI at the first and start publishing. See [docs/demo.md](./docs/demo.md) for the loaded patchbay and subjects worth subscribing to.

## Install

One-liner for Mac/Linux: downloads and unpacks the latest release into the current directory. See [scripts/start.sh](./scripts/start.sh)

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash
```

Or [grab the latest](https://github.com/lexvicacom/monoblok/releases/latest).


## patchbay

patchbay is a small S-expression DSL describing how every incoming publish gets filtered, conditioned, and re-routed. It is shared by the monoblok server and [tinyblok](https://github.com/lexvicacom/tinyblok) on MCUs.

Top-level forms are `(on SUBJECT-FILTER BODY)`. Wildcards are NATS-style: `*` matches one token, `>` matches the tail. EDN is canonical for hand-written patchbays; `.json` files are accepted for tooling compatibility.

```edn
(on "sensors.*"
  (when (> payload-float 30.0)
    (publish! (subject-append "high") payload)))

; Round to 1dp, drop duplicates, emit on the .stable sub-subject.
(on "sensors.*"
  (-> payload-float
      (round 1)
      (squelch)
      (publish! (subject-append "stable"))))
```

The vocabulary is borrowed from electronics (`squelch` suppresses until the value changes, `deadband` ignores movement smaller than a threshold) because the names already mean the right thing. A "patchbay" in a studio is a grid of jacks you wire between sources and destinations, which is exactly what the DSL looks like on the page.

JSON frames like `{"temp":12.5,"hum":80}` can be demuxed onto scalar sub-subjects (`json-demux!`) and conditioned the same way; dotted object paths are supported up to four levels deep. The root [`patchbay.edn`](./patchbay.edn) includes a starter rule for this, while [`json-frames.edn`](./examples/json-frames.edn) shows the fuller version.

Time-windowed `bar!` closes and `moving-* :ms` evictions are driven by one libxev timer per active slot, scheduled at the slot's exact next deadline; a quiet feed still flushes its bar at the window boundary, with no periodic walker.

The repository root [`patchbay.edn`](./patchbay.edn) is the short default tour: routing, filtering, numeric cleanup, LVC, JSON demux, and commented pointers to bars and bridging. Full reference and worked examples live in [docs/patchbay.md](./docs/patchbay.md). One-line summary of every form in the [cheatsheet](./docs/patchbay-cheatsheet.md). Runnable end-to-end demos are in [`examples/`](./examples/); each `.edn` has a matching `.sh` that starts monoblok, publishes a sequence, subscribes in parallel, and prints publishes vs deliveries. Larger, weirder Codex-generated patchbays live in [`advanced-examples/`](./advanced-examples/).

| file                                              | what it shows                                                   |
|---------------------------------------------------|-----------------------------------------------------------------|
| [`sensors.edn`](./examples/sensors.edn)           | round + squelch on a noisy sensor                               |
| [`office-temp.edn`](./examples/office-temp.edn)   | moving-average alert + all-clear via `transition` and `count!`  |
| [`ticker.edn`](./examples/ticker.edn)             | market data: round, squelch, big-jump alerts, bridge            |
| [`bars.edn`](./examples/bars.edn)                 | tick-count OHLC bars per symbol                                 |
| [`latency-stats.edn`](./examples/latency-stats.edn) | live p50/p95/p99/stddev over a sliding window                 |
| [`clocked.edn`](./examples/clocked.edn)             | silence detection, debounce, sampling, and clocked aggregates |
| [`json-frames.edn`](./examples/json-frames.edn)   | `json-demux!` a JSON-emitting device into scalar sub-subjects   |
| [`rental-car.edn`](./examples/rental-car.edn)     | quantize + deadband + over-rev hold-off alert                   |
| [`bridge.edn`](./examples/bridge.edn)             | forward selected subjects to a real NATS server                 |
| [`demo.edn`](./examples/demo.edn)                 | tour of every primitive on `demo.sensors.*`                     |
| [`lvc.edn`](./examples/lvc.edn)                   | `$LVC.>` cache replay: a late joiner gets the last value        |
| [`mixer.edn`](./examples/mixer.edn)               | mixer mode: one process fronts N workers, sharded by first token (run with `python3 examples/mixer.py`) |


### Testing

Run a patchbay directly with `monoblok examples/<file>.edn` or `.json`; form-lint without starting the server with `monoblok --validate examples/<file>.edn`.

For quick patchbay debugging, `--soundcheck` runs the same evaluator without opening a NATS socket. It reads newline-delimited `SUBJECT|payload` rows on stdin, passes inputs through stdout, and prints any `publish!` emissions. Time-based patchbay ops use the normal libxev clock path; after stdin closes, pending timers stay alive briefly unless you set `--soundcheck-linger-ms 0`.

```sh
printf 'sensors.temp|31\n' | monoblok --soundcheck examples/sensors.edn
printf 'sensors.temp|31\n' | monoblok --soundcheck --soundcheck-label examples/sensors.edn
```

### `--trace`: per-evaluation debugger

Prints every patchbay form the evaluator visits to stderr, with result and elapsed time. 

```
$ monoblok --port 4222 --patchbay patchbay.edn --trace
trace: sensors.temp 42.5
  rule 0 (on "sensors.*") matched
  (when (> payload-float 30) (publish! (subject-append "high") payload))
    (> payload-float 30)
      => true [124µs]
    (publish! (subject-append "high") payload)
      => published "sensors.temp.high" 42.5 [549µs]
total [3ms]
```

### Coding assistants

A nice way to learn it is with an LLM that has the DSL loaded as project context. [docs/AGENTS_PATCHBAY.md](./docs/AGENTS_PATCHBAY.md) is a self-contained, agent-neutral prompt that teaches Codex and other coding agents Patchbay. Append it to your project's `AGENTS.md` so compatible tools pick it up automatically when editing `.edn` rule files:

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/docs/AGENTS_PATCHBAY.md >> ./AGENTS.md
```

[docs/CLAUDE_PATCHBAY.md](./docs/CLAUDE_PATCHBAY.md) is the Claude-specific version for `CLAUDE.md`. Once loaded, describe the stream you have and the stream you want.

<p align="center">
  <img src="claude.png" alt="Claude Code editing a patchbay" width="720">
</p>

For some elaborate generated demos, see [`advanced-examples/`](./advanced-examples/). They are intentionally a bit over the top, but they give a feel for what is possible.

## `$LVC.*`: last-value-cache stream

Top-level `(lvc ...)` forms opt subjects into the last-value cache. Subscribing to `$LVC.foo.bar` joins a live stream of `foo.bar`: current cached value first (if any), then every subsequent opted-in publish. Wildcards work. `PUB $LVC.*` is rejected.

```edn
(lvc ["sensors.>" "alerts.>"])
```

```
PUB foo.bar 11      ; cache = 11
PUB foo.bar 12      ; cache = 12
SUB $LVC.foo.bar    ; -> immediately receives 12
PUB foo.bar 13      ; -> subscriber receives 13
```

Absent `(lvc ...)`, LVC is off and the publish path does no cache work. `--no-lvc` disables configured LVC filters.

## NATS support

### As a NATS core server

Supported: PUB / SUB / UNSUB / MSG, wildcards, request/reply, queue groups, headers.
Out of scope: TLS (use an NLB/HAProxy/nginx?), auth, JetStream, clustering et al. I think this fits the spirit of monoblok.

### As a bridging client to a NATS server

<p align="center">
  <img src="bridge.png" alt="bridge" width="720">
</p>

monoblok can forward a subset of local publishes to a real NATS cluster. **Export-only**: nothing flows in from the remote. TLS and `.creds` files are supported via vendored [nats.zig](https://github.com/nats-io/nats.zig).

Zero or one `(bridge ...)` form in the patchbay file configures it:

```edn
(bridge
  :servers  ["tls://connect.ngs.global:4222"]
  :creds    "/etc/monoblok/ngs.creds"
  :tls      true
  :name     "monoblok-prod-1"
  :export   ["telemetry.>" "alerts.>"])
```

A local publish (from a NATS client or a patchbay rule) whose subject matches any `:export` filter is forwarded as-is. Local subscribers are served first, bridge second, so a slow remote can't starve local delivery. Reconnects are handled inside nats.zig.

Full keyword reference (auth, timeouts, reconnect tuning) in [docs/patchbay-cheatsheet.md](./docs/patchbay-cheatsheet.md).

## Snapshots

Warm-start from disk so restarts don't lose the cache or the state inside gates and windows.

```
monoblok --snapshot /var/lib/monoblok/state.mblk --snapshot-every 10
```

`--snapshot PATH` loads on startup if it exists (missing is fine). `--snapshot-every SECONDS` runs a periodic background dump (atomic temp-file + rename, on a worker thread). `SIGINT` / `SIGTERM` always writes a final snapshot before exiting. If the patchbay file changes between runs, LVC entries still load; rule state for any rule 
whose filter no longer matches at its recorded position is skipped with a warning.

## Design

Each monoblok process owns one `xev.Loop` that owns accept, per-connection read/write completions, router state, and the LVC. Because everything happens on the loop thread, fan-out can append straight into each subscriber's outbound buffer with no locking and kick one `write` per connection per publish.

Everything application-level runs on a single thread: parsing, subject matching, rule evaluation, fan-out, write buffering. The kernel still uses your other cores for I/O, but once a byte arrives it's serial through monoblok. Adding a second thread would mean atomics or locks. The cap is one core's worth of throughput per instance, and the benchmarks below show that's a lot of headroom for signal conditioning workloads. 

[Mixer mode](./docs/mixer.md) reuses that same single-loop model per worker. The mixer-to-worker hop runs over inherited socketpairs rather than TCP or AF_UNIX, which simplifies things since the processes share a host.

## Deploying

monoblok has low hardware requirements. A 2-vCPU VM with 256 MB+ of RAM is a good starting point. monoblok runs on one core; the kernel net stack and io_uring workers will use the other.

The systemd unit plus `--snapshot` handles restarts: a crash or reboot loses at most one snapshot interval of in-flight conditioning state. If you're bridging upstream, that cluster can be thought of as the system of record (anything already exported is durable there).

### systemd

Linux release tarballs ship a unit file and installer in [scripts/](./scripts/):

```sh
sudo bash scripts/install-systemd.sh
sudo systemctl enable --now monoblok
journalctl -u monoblok -f
```

Drops the binary at `/usr/local/bin/monoblok`, the patchbay at `/etc/monoblok/patchbay.edn`, snapshots under `/var/lib/monoblok/state.mblk` (every 10s plus on stop), and creates a `monoblok` system user.

## Benchmarks

Getting meaningful numbers turned out to be trickier than I first realised. No specific percentages here; run `scripts/bench-with-nats-server.sh` on your own hardware if numbers matter to you.

The **shape** of the comparison vs. nats-server, though, is consistent across runs:

- **nats-server wins on pure publish throughput on big machines.** Multi-threaded acceptance and a more battle-hardened parse loop both help when there's no fan-out work to spread the cost over and there are spare cores to spread it across. On small ARM VPSes the gap narrows or disappears: nats-server has less parallelism to exploit, and monoblok's single-threaded design has no overhead to pay.
- **The two are roughly comparable at low fan-out** (1-10 subscribers per publish).
- **monoblok scales better with subscriber count.** The single-threaded deduped-kicks fan-out avoids the per-subscriber lock work a multi-threaded server pays. Crossover happens somewhere around 10-30 subscribers; the further past that you go, the bigger monoblok's lead.

Worth keeping in mind: nats-server has a decade of production-grade performance work behind it. Any monoblok win in these benches should be read as "the single-threaded design happens to fit this specific workload shape well," not "monoblok is faster than nats." The right tool for most pub/sub deployments is still nats-server; monoblok is for the cases where the patchbay or LVC features earn their place, and "fast enough on a small box" is a happy side-effect of the design, not the headline.

## Building from source

Zig 0.16.0 is required.
```
zig build --release=safe
./zig-out/bin/monoblok --port 4222 --patchbay patchbay.edn
```

Release binaries are built natively per arch by `.github/workflows/release.yml` on three runners: `ubuntu-22.04` (x86_64), `ubuntu-22.04-arm` (aarch64), `macos-latest` (aarch64).

## AI

It's 2026, Claude and Codex help me a lot. This is something of a _scarlet letter_ to many - [some thoughts on this](https://github.com/lexvicacom/monoblok/blob/main/docs/how-monoblok-uses-ai.md).

## License

MIT. See `LICENSE`.
