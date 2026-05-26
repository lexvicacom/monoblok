# monoblok Overview

> A NATS-core compatible messaging system that conditions subjects before subscribers see them.
> 
> Fix raw input streams once, not in every subscriber.

## Rationale

It is not uncommon for systems to contain some _caretaker_ services that subscribe to ingress NATS subjects to clean up and republish a raw stream before the real business starts. This might include rounding, dedup, deadband, JSON demux, OHLC bars, threshold alerts and so on. High velocity or miniscule changes don't always have value downstream. monoblok lets you declare that tidying work once, leveraging efficient implementations of common tasks as rules at the broker, instead of writing _rounding logic_ N times in N services.

**Declare it once, as rules, in the broker.**

monoblok speaks NATS. Point your NATS clients at it and the conditioning happens on the way through. Rules live in patchbay, a small S-expression DSL.

![monoblok round and squelch demo](./monoblok-round-squelch-fixed.gif)

Common ways of running monoblok:
- Standalone broker: clients connect directly to monoblok for lightweight NATS-core pub/sub with signal conditioning built in.
- Signal conditioning front door: publishers send raw events to monoblok, monoblok cleans them, then forwards selected subjects to a real NATS cluster.
- Tap into existing NATS: monoblok subscribes to selected subjects on a real NATS cluster, treats them as private patchbay input, then emits only the cleaned or derived subjects your rules choose.

![monoblok deployment modes](./infographic.png)

monoblok is written in C with libuv and builds on Linux and macOS. It aims to be **fast**, even on entry level/shared hardware. The [saved benchmarks](../bench-results) show low millions of msgs/sec on a 2-core ARM VPS, suggesting monoblok is unlikely to be the bottleneck in many likely conditioning workloads. The numbers are directional rather than scientific; see the [scripts](../scripts) if you want to rerun them.

[tinyblok](https://github.com/lexvicacom/tinyblok) is an implementation of the same idea, but for microcontrollers.


[Read the introductory blog post](https://alexjreid.dev/posts/monoblok/) [and friends](https://alexjreid.dev/tags/monoblok/).

## Public demo server

A [public demo server](https://alexjreid.dev/posts/monoblok-demo/) runs on `demo.monoblok.host:4222`, with a bridged NATS server on `demo.monoblok.host:4223`. All you need is the NATS CLI. The loaded patchbay is [`demo.edn`](../examples/demo.edn) - if this makes no sense, don't worry, patchbay will be introduced later in this document.

Open a few terminals:

```sh
nats -s demo.monoblok.host:4222 sub 'demo.sensors.>'
(new terminal)
nats -s demo.monoblok.host:4223 sub '>'
(new terminal)
nats -s demo.monoblok.host:4222 pub demo.sensors.temp 21.001
nats -s demo.monoblok.host:4222 pub demo.sensors.temp 21.002
nats -s demo.monoblok.host:4222 pub demo.sensors.temp 2331.104
nats -s demo.monoblok.host:4222 pub demo.sensors.temp 21.104
```

Conditioned values are visible on the first subscription, as well as the input to `demo.sensors.temp` due to our subscription filter. As the `2331.104` value breaches a threshold, it is also exported to NATS so is visible on the second subscription.

See [docs/demo.md](./demo.md) for the loaded patchbay and subjects worth subscribing to.

## Install

### Binary (macOS/Linux)
This script downloads and unpacks the latest release into the current directory. See [scripts/start.sh](../scripts/start.sh)

You can run it with:

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash
```

Or [grab the latest](https://github.com/lexvicacom/monoblok/releases/latest).

## patchbay

patchbay is a small S-expression DSL describing how every incoming publish gets filtered, conditioned, and re-routed. It is shared by the monoblok server and [tinyblok](https://github.com/lexvicacom/tinyblok) on MCUs.

Rules are top-level `(on SUBJECT-FILTER BODY)` forms. Config forms such as `(lvc ...)` and `(export ...)` live at top level too. Wildcards are NATS-style: `*` matches one token, `>` matches the tail. EDN is canonical for hand-written patchbays; `.json` files are accepted for tooling compatibility, and `.yml` / `.yaml` files can use the small patchbay YAML sugar layer.


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

JSON payloads like `{"temp":12.5,"hum":80}` can be demuxed onto scalar sub-subjects (`json-demux!`) and conditioned the same way; dotted object paths are supported up to four levels deep. [`json-frames.edn`](../examples/json-frames.edn) shows the fuller version.

Time-windowed operators cover OHLC bars, moving aggregates, silence detection, debounce, and sampling.

The root [`patchbay.edn`](../patchbay.edn) is the short tour. Full syntax lives in [docs/patchbay.md](./patchbay.md), with a one-line operator summary in [docs/patchbay-cheatsheet.md](./patchbay-cheatsheet.md). Runnable examples live in [`examples/`](../examples/).

| file                                              | what it shows                                                   |
|---------------------------------------------------|-----------------------------------------------------------------|
| [`sensors.edn`](../examples/sensors.edn)           | round + squelch on a noisy sensor                               |
| [`office-temp.edn`](../examples/office-temp.edn)   | moving-average alert + all-clear via `transition` and `count!`  |
| [`ticker.edn`](../examples/ticker.edn)             | market data: round, squelch, big-jump alerts, export            |
| [`bars.edn`](../examples/bars.edn)                 | tick-count OHLC bars per symbol                                 |
| [`latency-stats.edn`](../examples/latency-stats.edn) | live p50/p95/p99/stddev over a sliding window                 |
| [`clocked.edn`](../examples/clocked.edn)             | silence detection, debounce, sampling, and clocked aggregates |
| [`json-frames.edn`](../examples/json-frames.edn)   | `json-demux!` a JSON-emitting device into scalar sub-subjects   |
| [`rental-car.edn`](../examples/rental-car.edn)     | quantize + deadband + over-rev hold-off alert                   |
| [`rental-car.yml`](../examples/rental-car.yml)     | the same rules using YAML sugar                                 |
| [`bridge.edn`](../examples/bridge.edn)             | export selected subjects to a real NATS server                  |
| [`jetstream.yml`](../examples/jetstream.yml)       | replay a populated JetStream stream, then continue live         |
| [`demo.edn`](../examples/demo.edn)                 | tour of every primitive on `demo.sensors.*`                     |
| [`lvc.edn`](../examples/lvc.edn)                   | `$LVC.>` cache replay: a late joiner gets the last value        |


### Validate and debug rules

Run a patchbay directly with `monoblok examples/<file>.edn`, `.json`, or `.yml`; form-lint without starting the server with `monoblok --validate examples/<file>.edn`.

For quick patchbay debugging, `--soundcheck` runs the same evaluator without opening a NATS socket. It reads newline-delimited `SUBJECT|payload` rows on stdin, passes inputs through stdout, and prints any `publish!` emissions. Clock/window state lingers briefly after EOF so delayed forms can close; use `--soundcheck-linger-ms 0` to disable that wait.

```sh
printf 'sensors.temp|31\n' | monoblok --soundcheck examples/sensors.edn
printf 'sensors.temp|31\n' | monoblok --soundcheck --soundcheck-label examples/sensors.edn
```

### `--trace`: protocol trace

Logs parsed client operations to stderr. For rule-level debugging, use `--soundcheck` or a temporary `(print! ...)` inside the patchbay.

```
$ monoblok --port 4222 --patchbay patchbay.edn --trace
trace: conn 1 CONNECT
trace: conn 1 SUB sensors.> 1
trace: conn 2 PUB sensors.temp 4
```

### Coding assistants

A nice way to learn it is with an LLM that has the DSL loaded as project context. [docs/AGENTS_PATCHBAY.md](./AGENTS_PATCHBAY.md) is a self-contained, agent-neutral prompt that teaches Codex and other coding agents Patchbay. Append it to your project's `AGENTS.md` so compatible tools pick it up automatically when editing `.edn` rule files:

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/docs/AGENTS_PATCHBAY.md >> ./AGENTS.md
```

[docs/CLAUDE_PATCHBAY.md](./CLAUDE_PATCHBAY.md) is the Claude-specific version for `CLAUDE.md`. Once loaded, describe the stream you have and the stream you want.

<p align="center">
  <img src="claude.png" alt="Claude Code editing a patchbay" width="720">
</p>

For some elaborate generated demos, see [`advanced-examples/`](../advanced-examples/). They are intentionally a bit over the top, but they give a feel for what is possible.

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

`$STATS.*` can be cached too, but only with an explicit stats filter such as `(lvc ["$STATS.>"])`. A broad `(lvc [">"])` keeps stats out of the LVC unless you opt in.

## `$STATS.*`: live counters

The server publishes cumulative decimal counters on `$STATS.*` every 60 seconds by default. Use `--stats-tick-ms MS` to change the cadence.

| subject | meaning |
|---|---|
| `$STATS.global.pubs` | accepted inbound client PUBs since start |
| `$STATS.rules.<i>.emitted` | successful `publish!`-style emits by rule, 0-based |
| `$STATS.rules.<i>.suppressed` | gate/window suppressions by rule, 0-based |
| `$STATS.bridge.published` | publishes forwarded to the remote NATS cluster |
| `$STATS.bridge.dropped` | bridge publishes that failed or were dropped |
| `$STATS.import.received` | remote NATS messages accepted by import |
| `$STATS.import.processed` | imported messages evaluated by patchbay |
| `$STATS.import.dropped` | imported messages dropped before evaluation |
| `$STATS.import.failed` | imported messages whose patchbay evaluation failed |

Client publishes to `$STATS.*` are rejected. Subscribe to `$STATS.>` for the live stream, or add `(lvc ["$STATS.>"])` if late joiners should receive the most recent tick immediately.

## NATS support

monoblok implements the NATS core pieces it needs to behave like a small broker.

| Feature | Support |
|---|---|
| `PUB` / `SUB` / `UNSUB` / `MSG` | yes |
| wildcards | yes |
| request/reply / reply-to | yes, local core reply-to; bridge remains export-only |
| queue groups | yes |
| headers | no |
| `$LVC.*` last-value replay | yes, monoblok extension |
| `$STATS.*` live counters | yes, monoblok extension |
| bridge to real NATS | export-only |
| import from real NATS | yes, as private patchbay ingress |
| JetStream import | yes, as consumer-only patchbay ingress |
| TLS on the local server | yes, optional server cert/key |
| auth on the local server | yes, optional global token or user/pass from environment |
| HTTP/SSE adapter | yes, optional plain HTTP listener |
| JetStream service for local clients | no |
| clustering | no |

### Simple auth for local NATS clients

Local client auth is optional and global. Configure either a token or a single
username/password pair by naming the environment variables that hold the
expected secret values:

```sh
MONOBLOK_AUTH_TOKEN='sekret' \
  monoblok --port 4222 --patchbay patchbay.edn \
    --auth-token-env MONOBLOK_AUTH_TOKEN

MONOBLOK_AUTH_USER='alice' MONOBLOK_AUTH_PASS='wonder' \
  monoblok --port 4222 --patchbay patchbay.edn \
    --auth-user-env MONOBLOK_AUTH_USER \
    --auth-pass-env MONOBLOK_AUTH_PASS
```

With auth enabled, `INFO` advertises `"auth_required":true`. Clients must send
the normal NATS `CONNECT` fields: `auth_token` for token auth, or `user` and
`pass` for username/password auth. Missing or wrong credentials receive
`-ERR 'Authorization Violation'` and the socket is closed.

This is intentionally only simple CONNECT-time authentication. It does not add
per-subject authorization, accounts, bcrypt hashes, JWTs, NKEYs, or reloadable
user maps. Use TLS, a trusted network, or a fronting proxy if credentials cross
anything outside your control.

### TLS for local NATS clients

Server-side TLS is optional. Start monoblok with a PEM certificate chain and
matching private key:

```sh
monoblok --port 4222 --patchbay patchbay.edn \
  --tls-cert /etc/monoblok/server.crt \
  --tls-key /etc/monoblok/server.key
```

monoblok follows the normal NATS TLS upgrade flow: the accepted socket first
receives a plaintext `INFO` containing `"tls_required":true`, then the client
starts TLS on that same socket before sending `CONNECT`. Non-TLS clients will
not be able to complete the connection once TLS is enabled.

Use a certificate trusted by your clients in production. For private/self-signed
certificates, configure clients with the CA certificate where possible. Test
clients can disable verification, but that is insecure and should stay out of
production. With `nats.c`, the development-only equivalent is
`natsOptions_SkipServerVerification(opts, true)`.

### HTTP/SSE adapter

The optional HTTP listener is a small browser-friendly adapter over the same
local client publish and subscribe paths. It is disabled by default:

```sh
monoblok --port 4222 --patchbay patchbay.edn \
  --http-host 127.0.0.1 \
  --http-port 8080
```

Routes use path segments as NATS subject tokens:

```text
GET  /sub/sensors/temp       -> SSE subscription to sensors.temp
GET  /sub/sensors/%3E        -> SSE subscription to sensors.>
GET  /sub/$LVC/sensors/temp  -> SSE subscription to $LVC.sensors.temp
GET  /latest/sensors/temp    -> one-shot LVC lookup for sensors.temp
POST /pub/sensors/temp       -> client publish to sensors.temp
```

`GET /latest/<subject>` returns the cached payload for that exact, unprefixed
subject as `Content-Type: text/plain; charset=utf-8`, with no JSON wrapper. It
returns `404` when LVC has no value for the subject and `409` when LVC is
disabled. Use `/sub/$LVC/...` for wildcard replay or for a stream that stays
open for future updates.

`POST` requires `Content-Length` plus `Content-Type: text/plain` or
`Content-Type: application/json`, and rejects NUL bytes. It is intentionally a
text adapter, not a binary payload API. Successful POSTs fan out exactly like a
local NATS `PUB`: normal subscribers, SSE subscribers, LVC, bridge, and
patchbay all see the same publish.

SSE messages are JSON data envelopes:

```text
event: msg
data: {"subject":"sensors.temp","payload":"31.2"}

```

When local auth is configured, HTTP uses `Authorization: Bearer <token>` for
token mode or `Authorization: Basic ...` for user/pass mode. The HTTP listener
is plain HTTP only; put Caddy, nginx, or another proxy in front for HTTPS. A
tiny fetch-based JavaScript helper lives at
[`examples/http-sse-client.js`](../examples/http-sse-client.js), with a browser
page at [`examples/http-sse-client.html`](../examples/http-sse-client.html).

#### nginx reverse proxy for SSE

Run monoblok's HTTP listener on loopback and let nginx terminate public TLS:

```sh
monoblok --port 4222 --patchbay patchbay.edn \
  --http-host 127.0.0.1 \
  --http-port 8080
```

Use a `proxy_pass` without a URI suffix so encoded subject tokens such as
`%3E` stay intact. SSE subscriptions also need buffering and compression
disabled, with a read timeout longer than the longest expected quiet period:

```nginx
upstream monoblok_http {
    server 127.0.0.1:8080;
}

server {
    listen 443 ssl;
    server_name events.example.com;

    ssl_certificate     /etc/letsencrypt/live/events.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/events.example.com/privkey.pem;

    location /sub/ {
        proxy_pass http://monoblok_http;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Connection "";

        proxy_buffering off;
        proxy_cache off;
        gzip off;
        proxy_read_timeout 1h;
        proxy_send_timeout 1h;
    }

    location /pub/ {
        client_max_body_size 1m;

        proxy_pass http://monoblok_http;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Connection "";
    }

    location /latest/ {
        proxy_pass http://monoblok_http;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Connection "";
    }
}
```

Serve browser clients from the same nginx origin when possible. monoblok does
not add CORS headers itself, and same-origin `/sub/...`, `/pub/...`, and
`/latest/...` requests avoid needing browser-specific CORS handling. If monoblok
auth is enabled, make sure nginx forwards the `Authorization` header; the
default proxy behavior does unless you override it.

Use the Node demo server to serve the page and proxy `/sub`, `/pub`, and
`/latest` from the same origin, which avoids browser CORS. It can also start a local monoblok
process for the demo:

```sh
node examples/http-sse-demo.js --start-monoblok --port 8090 --open
```

### As a bridging/importing client to a NATS server

<p align="center">
  <img src="bridge.png" alt="bridge" width="720">
</p>

monoblok can forward a subset of local publishes to a real NATS cluster, or
subscribe to remote subjects and run those messages through patchbay. TLS and
`.creds` files are supported via vendored [nats.c](https://github.com/nats-io/nats.c).

Zero or one `(export ...)` form in the patchbay file configures it. The old
`(bridge ...)` spelling is deprecated but still accepted:

```edn
(export
  :servers  ["tls://connect.ngs.global:4222"]
  :creds    "/etc/monoblok/ngs.creds"
  :tls      true
  :name     "monoblok-prod-1"
  :origin-header true
  :replay-header true
  :export   ["telemetry.>" "alerts.>"])
```

A local publish (from a NATS client or a patchbay rule) whose subject matches any `:export` filter is forwarded as-is. With `:origin-header true`, forwarded messages also carry `x-monoblok: <hostname>` for remote-side provenance, including replay-derived publishes bridged during JetStream catch-up. With `:replay-header true`, replay-derived bridge publishes carry `x-monoblok-replay: true` and `x-monoblok-assumed-ts: <unix-ms>`, the wall timestamp used for replay evaluation; after catch-up, live bridge publishes do not carry either replay header. Local subscribers are served first, bridge second, so a slow remote can't starve local delivery. Reconnects are handled inside nats.c.

Zero or one `(import ...)` form configures inbound tap mode. The flat import
shape is a compatibility alias for one core NATS import:

```edn
(import
  :servers ["nats://raw.example:4222"]
  :name    "monoblok-import-prod-1"
  :subject ["raw.>" "replay.>"])
```

Imported messages are patchbay inputs only. Direct monoblok subscribers do not
see the raw imported subject unless a rule republishes it. In import mode, the
local socket remains available for subscribers, but client `PUB` commands are
rejected.

Newer configs can make the import kind explicit. `:core` entries are live core
NATS subscriptions; `:streams` entries are JetStream durable consumers:

```edn
(import
  :core
  [[:servers ["nats://raw.example:4222"]
    :subject ["raw.>"]]]

  :streams
  [[:servers ["nats://js.example:4222"]
    :subject "sensors.temp"
    :stream "SENSORS"
    :consumer "monoblok-prod-1"
    :catch-up true]])
```

With `:streams`, monoblok consumes JetStream; it does not serve JetStream to
local clients. On startup it can replay historical messages before opening its
listener, using JetStream message metadata as the event-time clock. During that
loading phase, rules can branch on `replaying?` to warm LVC, suppress historical
output, or publish replay output to a separate subject. See
[`examples/jetstream.yml`](../examples/jetstream.yml) and
[`examples/jetstream.sh`](../examples/jetstream.sh), which starts a JetStream
server on `JS_PORT` (default `15889`), starts a plain NATS bridge target on
`BRIDGE_PORT` (default `15890`), exports `JS_URL` / `BRIDGE_URL`, and populates
the stream with `COUNT=1000` events by default. The example pauses once during
population so JetStream replay has enough event-time gap to close a `bar! :ms`
window during catch-up, and the bridge target shows replay-derived exports
while monoblok's local listener is still closed.

Full keyword reference (auth, timeouts, reconnect tuning) in [docs/patchbay-cheatsheet.md](./patchbay-cheatsheet.md).

## Snapshots

Warm-start from disk so restarts don't lose the cache or the state inside gates and windows.

```
monoblok --snapshot /var/lib/monoblok/state.mblk --snapshot-every 10
```

`--snapshot PATH` loads on startup if it exists (missing is fine). `--snapshot-every SECONDS` runs a periodic dump using atomic temp-file + rename. If the patchbay file changes between runs, LVC entries still load; rule state for any rule 
whose filter no longer matches at its recorded position is skipped with a warning.

Patchbay state is usually per rule and per subject. Avoid unbounded subject tokens such as timestamps, user IDs, or raw device-generated strings unless you intend to create state for each one.

## Design

monoblok is intentionally simple at runtime: one process, one libuv loop, one application thread for protocol parsing, subject matching, patchbay evaluation, fan-out, write buffering, and LVC state. That makes the behavior predictable and keeps coordination costs out of the publish path.

For lower-level details on ownership boundaries, snapshot I/O, libuv behavior, borrowed slices, and allocation policy, see [implementation notes](./implementation-notes.md).

## Deploying

monoblok has low hardware requirements. A 2-vCPU VM with 256 MB+ of RAM is a good starting point. monoblok runs on one application core; the operating system can still use other cores for networking and file I/O.

The systemd unit plus `--snapshot` handles restarts: a crash or reboot loses at most one snapshot interval of in-flight conditioning state. If you're bridging upstream, that cluster can be thought of as the system of record (anything already exported is durable there).

### systemd

Linux release tarballs ship a unit file and installer in [scripts/](../scripts/):

```sh
sudo bash scripts/install-systemd.sh
sudo systemctl enable --now monoblok
journalctl -u monoblok -f
```

Drops the binary at `/usr/local/bin/monoblok`, the patchbay at `/etc/monoblok/patchbay.edn`, snapshots under `/var/lib/monoblok/state.mblk` (every 10s plus on stop), and creates a `monoblok` system user.

## How fast is it?

tl;dr: It's likely to be fast enough. The saved low-end VPS run is already in the low millions of messages/sec for simple pub/sub and fan-out cases. Getting meaningful benchmarks turned out to be trickier than I first realised, though, so run `scripts/bench-with-nats-server.sh` on your own hardware if numbers matter to you.

The **shape** of the comparison vs. nats-server, though, is consistent across runs:

- **nats-server wins on pure publish throughput on big machines.** Multi-threaded acceptance and a more battle-hardened parse loop both help when there's no fan-out work to spread the cost over and there are spare cores to spread it across. On small ARM VPSes the gap narrows or disappears: nats-server has less parallelism to exploit, and monoblok has less coordination overhead to pay.
- **The two are roughly comparable at low fan-out** (1-10 subscribers per publish).
- **monoblok scales better with subscriber count.** Its fan-out model has low per-subscriber coordination overhead. Crossover happens somewhere around 10-30 subscribers; the further past that you go, the bigger monoblok's lead.

Worth keeping in mind: nats-server has a decade of production-grade performance work behind it. Any monoblok win in these benches should be read as "the single-threaded design happens to fit this specific workload shape well," not "monoblok is faster than nats." The right tool for most pub/sub deployments is still nats-server; monoblok is for the cases where the patchbay or LVC features earn their place, and "fast enough on a small box" is a happy side-effect of the design, not the headline.

## Building from source

See the repository [README](../README.md) for CMake build targets and smoke tests.

## License

MIT. See `LICENSE`.
