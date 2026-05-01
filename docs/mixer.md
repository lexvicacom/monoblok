# Mixer mode

`monoblok --mixer cfg.edn` switches the binary's role from "NATS
daemon" to "stateless front-end that fronts N worker daemons." One
binary, one config file, one systemd unit. Workers are normal monoblok
processes; they don't know about the mixer beyond the inherited fd
they were handed.

The point is to partition a noisy inbound stream across cores when one
core could saturate: each worker handles its own shard's ingest,
parsing, and rule evaluation on its own core, while the mixer just
routes bytes. Build-out before build-across: start with one monoblok
and only reach for the mixer when one core actually isn't enough.

For background and design notes, see the blog post: [monoblok mixer
mode](https://alexjreid.dev/posts/monoblok-mixer/).

This is experimental. Subjects-as-data and the routing DSL are stable;
the mixer is "works for the cases I've used" rather than "every
NATS-protocol corner is covered."

## Config

A single `(mixer ...)` form in the config file:

```edn
(mixer
  :listen "tcp://0.0.0.0:4222"
  :workers
    [[:shard "SENSORS" :patchbay "examples/mixer-sensors.edn"]
     [:shard "ORDERS"  :patchbay "examples/mixer-orders.edn"]
     [:shard "*"       :patchbay "examples/mixer-default.edn"]])
```

| keyword          | type              | meaning                                                              |
|------------------|-------------------|----------------------------------------------------------------------|
| `:listen`        | string            | client-facing address. Currently `tcp://HOST:PORT` only.             |
| `:workers`       | vector of workers | each worker is a vector of `:shard` / `:patchbay` / `:log` / `:trace` |

Per-worker keywords:

| keyword     | type   | meaning                                                                  |
|-------------|--------|--------------------------------------------------------------------------|
| `:shard`    | string | first-subject-token this worker owns. Exactly one `"*"` (catch-all) required. |
| `:patchbay` | string | path to the worker's patchbay file                                       |
| `:log`      | string | optional. `dup2`'d onto fd 2 before exec so the worker's stderr lands here. |
| `:trace`    | bool   | optional. Pass `--trace` on the worker's CLI.                            |

Worker entries also accept the legacy `(...)` list form, but `[...]`
is the recommended shape.

## Sharding rule

First subject token only. The mixer takes everything before the first
`.`, looks it up in the worker table, and forwards there. Subjects
with no dot are treated as their own first token.

This is operator-picked partitioning, not automatic rebalancing.
Namespace by domain (`SENSORS.*`, `ORDERS.*`, `MARKET.*`) and decide
which shard owns which subtree. There is no fan-out across workers and
no rebalancing.

Two SUB constraints fall out of this:

- **Wildcards in the first token are rejected on `SUB`.** `SUB *.foo
  3` would match every shard; the mixer can't pick one without
  fanning, and fanning would put each client on N upstream subs.
- **`SUB >` (whole firehose) returns `-ERR`.** Same reason.

Subscribe to a specific shard's subtree (`SUB SENSORS.>`) and you're
fine.

## SUB coalescing

The mixer keeps one upstream SUB per unique filter, no matter how many
clients want it. First client to `SUB foo.> 1` allocates a filter
entry, mints a fresh internal sid, and forwards `SUB foo.> <internal>`
upstream. A second client doing `SUB foo.> 7` finds the existing entry
and just appends itself to the subscriber list (no upstream traffic).
Worker fan-out cost scales with unique filters, not subscribers. A
hundred dashboards on the same filter look like one to the worker.

`UNSUB` removes one subscriber; the upstream SUB only goes away when
the last subscriber drops it. Client disconnect walks the per-client
sub map and decrements every entry the same way.

## Supervisor policy

Deliberately minimal:

- Spawn at startup. The mixer creates a `socketpair(AF_UNIX,
  SOCK_STREAM)` per worker, `fork+execvp`s `monoblok` with `--inherit-fd
  3`, and the worker `dup2`s its half onto fd 3 before exec.
  `Server.serveInheritedFd` wraps it as a single pre-connected
  connection (no listen, no accept; the mixer is the worker's sole
  peer for life).
- If any worker EOFs its upstream socket, the mixer exits the loop and
  the process dies. systemd restarts the whole tree.
- Mixer SIGTERM cascades via `kill(pid, SIGTERM)` on each worker.
- No SIGCHLD restart loop, no backoff, no per-worker health checks.

The "any worker dies, mixer dies" rule is the load-bearing
simplification: there's no half-up tree to reason about.

## Out of scope (v1)

- **`$LVC.*` cache replay through the mixer.** A worker's `SUB
  $LVC.foo` would route to the worker that owns `foo`'s shard;
  untested. Each worker has its own `$LVC` for its own shard's
  subjects.
- **`UNSUB sid N` (auto-after-N-msgs).** Would require per-sub
  counters at the mixer.
- **Aggregating `$STATS.*` across workers.** Each worker emits its own
  `$STATS.*` independently.
- **Listen schemes other than `tcp://`.** No DNS, no AF_UNIX on the
  client-facing side yet.

## Multi-host

If your NATS subject space is organised hierarchically, the same
first-token discipline scales past one box: run independent monobloks
on different machines, each handling a subtree of the hierarchy with
hardware provisioned to suit. It's not a cluster or HA story; if any
process in a mixer tree dies, systemd restarts the lot.

## Files and examples

- [`examples/mixer.edn`](../examples/mixer.edn): the mixer config
- [`examples/mixer-sensors.edn`](../examples/mixer-sensors.edn),
  [`mixer-orders.edn`](../examples/mixer-orders.edn),
  [`mixer-default.edn`](../examples/mixer-default.edn): per-worker
  patchbays
- [`examples/mixer.py`](../examples/mixer.py): runnable end-to-end demo
- [`scripts/mixer-smoke.sh`](../scripts/mixer-smoke.sh): PUB-routing
  smoke test
- [`scripts/mixer-sub-smoke.py`](../scripts/mixer-sub-smoke.py):
  SUB/UNSUB/coalescing smoke test (Python because nc's timing is too
  coarse for multi-subscriber sequencing)

Implementation lives in `src/mixer.zig` (mixer + worker + client conn +
filter entry) and `src/mixer_config.zig` (the config loader).
