# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and test

```
zig build                    # debug build (bridge on, requires system OpenSSL)
zig build -Dbridge=false     # debug build without the NATS bridge
zig build --release=safe     # release build (what CI ships)
zig build --release=fast     # release build (use this for any benchmarking)
zig build test               # unit tests (proto, subject, sexpr, rules, bridge)
bash scripts/smoke.sh        # end-to-end test: spins up daemon, drives over raw TCP with nc
bash scripts/bridge-smoke.sh # end-to-end bridge test: spins up a real nats-server, monoblok, and verifies local->remote forwarding
```

Release binaries are built natively on per-arch runners (see `.github/workflows/release.yml`): one each for linux-x86_64 (ubuntu-22.04), linux-aarch64 (ubuntu-22.04-arm), and macos-aarch64. There is no cross-compile step; each job just runs `zig build --release=safe` with OpenSSL installed from the local package manager. This is deliberate — dyn-linking against system OpenSSL is much simpler per-arch than cross-compiling it.

The bridge requires OpenSSL headers/libs at build time. On macOS: `brew install openssl@3`. On Debian/Ubuntu: `apt install libssl-dev`. Resolved via pkg-config.

There is no separate test runner. Individual unit tests live in `test "..."` blocks at the bottom of each source file; `zig build test` runs them all. To focus on one file's tests during development, comment the others out of `src/main.zig`'s trailing `test {}` block — there's no `--test-filter` wired in.

Zig version: **0.16.0** exactly. The code assumes its stdlib shape.

Running the daemon:

```
./zig-out/bin/monoblok --port 4222 --patchbay patchbay.edn        # LVC on (default)
./zig-out/bin/monoblok --port 4222 --patchbay patchbay.edn --no-lvc
./zig-out/bin/monoblok --port 4222 --patchbay patchbay.edn --stats  # log running max rule-publishes-per-input and per-conn outbound hwm every 10k PUBs
```

The routing DSL is called the **patchbay**; its file is `patchbay.edn`. The CLI flag is `--patchbay`, with `--rules` kept as a silent backwards-compatible alias. Internal code still uses the generic names `Rule`, `rules.zig`, `loadRules` — those are implementation terms, not user-facing vocabulary.

## Architecture

**Single-threaded libxev event loop.** One `xev.Loop` owns everything: the accept completion, per-connection read/write completions, router state, the LVC. No mutexes, no atomics on the hot path. The one hard invariant: every callback that mutates shared state runs on the loop thread.

**Why libxev and not `std.Io`:** Tried it but Zig 0.16.0's `std.Io` networking backends are all broken for our targets — `Dispatch` (macOS) has no net ops, `Kqueue` references a vtable field that no longer exists, `Uring` has error-set mismatches that stop compilation. libxev has working kqueue/io_uring/epoll/iocp. `std.Io` is still used at startup for loading the rules file (file ops work fine) — see `main.zig`'s `readFile`.

**Module layout and dependency direction:**

- `subject.zig` — pure: validates subjects, wildcard matcher (`*` token, `>` tail)
- `proto.zig` — pure, slice-based NATS wire parser + serializers that append into a caller's `ArrayList(u8)`. Returns `ParseResult { op, consumed }` or `error.NeedMoreData`. Zero allocations on parse.
- `sexpr.zig` — pure s-expression parser; arena-allocated output
- `rules.zig` — depends on sexpr + subject. Compiles rule file into `[]Rule`; `run()` evaluates bodies against a `Context { subject, payload, publisher, arena, gpa, current_rule }`. The split between `arena` (per-message scratch, reset each publish) and `gpa` (long-lived, owns the per-rule state tables for `squelch` / `deadband` / `moving-*`) is load-bearing — stateful ops must use `gpa` for anything that outlives a single message.
- `router.zig` — depends on proto + subject + rules. Owns `Router` (subscription table + LVC `StringHashMap` with value-buffer reuse) and `Conn` (router-facing side of a connection: outbound `ArrayList(u8)`, a `kick_fn` callback that notifies the server when there are bytes to write)
- `server.zig` — depends on everything. libxev state machine. Each connection = one `Conn` struct holding the `xev.TCP`, rx buffer, in-flight write buffer, and its `router.Conn`. On read completion: `proto.parseClientOp` in a loop until `NeedMoreData`, dispatch each op, compact the rx buffer. On each op that produces output: append to `router_conn.out`, then `maybeKickWrite` (if no write in flight, swap `out` with `in_flight_buf`, fire a `tcp.write`). Partial writes resubmit the remainder on the same completion.
- `main.zig` — CLI parsing, rule file load, loop construction, `server_id` generation

**The router/server split is load-bearing.** Router has no libxev knowledge; it just asks `Conn` to accept bytes and calls `conn.kick()`. Server owns the kick callback and translates it into a libxev write completion. This keeps `router.zig` testable without the event loop and would make swapping libxev out (again) survivable.

**Fan-out write pattern:** router iterates subscriptions, appends MSG bytes to each matching conn's outbound buffer, then after the loop kicks each conn at most once (deduped). The server's `maybeKickWrite` uses one completion per conn, partial-writes handled in the write callback. Do **not** use libxev's `queueWrite` / `WriteQueue` — we tried, and reusing a single `WriteRequest` racing with in-flight writes produces `invalid state in submission queue state=.active` spam. The current non-queued `write` with a `write_in_flight` guard is deliberate.

**Conn lifetime:** `router_mod.Conn` has a refcount. The server retains it at create, releases on close. Router fan-out does not currently retain (single-threaded: fan-out finishes before the loop can deliver a close). `markClosed()` is called before `removeAllFor()` so any in-flight fan-out that touches a closing conn becomes a no-op. The primitives are there if threading gets reintroduced.

**Per-message scratch:** `Conn.msg_arena` is reset at the start of each `PUB` dispatch. Rule evaluation and subject/payload copies live there. Router fan-out uses its own arena per publish, free-on-scope-exit. LVC values use reusable `ArrayList(u8)`s inside the hashmap (clear + appendSlice instead of free + dupe).

## `$LVC.*` — last-value streams

`$LVC.<subject>` is a live stream: subscribing registers a normal sub whose match filter is the stripped inner subject, plus an `is_lvc` flag. On subscribe, any matching cached value is emitted immediately. On each subsequent publish to the inner subject, router fan-out emits a MSG prefixed with `$LVC.`. Publishing to `$LVC.*` is rejected by the server as read-only. `--no-lvc` disables the cache + rejects `$LVC.*` subscribes.

## `$STATS.*` — live counters

An `xev.Timer` in the Server fires every `stats_tick_ms` (1s) and calls `emitStats`, which publishes cumulative u64 totals to `$STATS.global.pubs`, `$STATS.rules.<i>.emitted`, `$STATS.rules.<i>.suppressed` via `router.publish`. Rules are indexed by position in the loaded patchbay array (stable). Counters live on `Rule` (`publishes_emitted`, `publishes_suppressed`) and `Server.total_pubs`; `publishes_emitted` is bumped inside `callPublish` / `callPublishTo`, `publishes_suppressed` inside the gates (`squelch`, `deadband`, `changed?`, `rising-edge`, `falling-edge`) whenever they return nil (or `changed?` returns false). The stream goes through normal fan-out + LVC, so `SUB $LVC.$STATS.>` replays the last tick on subscribe. Client publishes to `$STATS.*` are rejected at the same place `$LVC.*` is, in `pub_msg`.

## Patchbay (routing DSL)

Top-level forms are `(on SUBJECT-FILTER BODY)`. The evaluator sees these bound symbols: `subject`, `payload`, `payload-float`. Functions: `publish`, `publish-to`, `subject-append`, `str-concat`, `contains?`, `not`, arithmetic (`+ - * /`), comparisons (`= < <= > >=`), numeric transforms (`round`, `quantize`), stateful gates (`squelch`, `deadband` — each per-rule-per-subject, stored in `Rule.state`), windowed aggregates (`moving-avg`, `moving-sum`, `moving-max`, `moving-min` — fixed-size ring per `(rule, subject, op)` slot), and edge gates (`rising-edge`, `falling-edge` — fire once per boolean transition, keyed per `(rule, op, subject)`, first sight never fires). Special forms: `if`, `when`, `and`, `or`, `do`, `->`, `transition` (fires a rising-branch on false→true, a falling-branch on true→false, nil otherwise; one shared `prev` slot per (rule, subject), distinct from `rising-edge`/`falling-edge` state). **Messages published from the patchbay go through normal fan-out but do not re-enter the DSL** (no loops). See `patchbay.edn` and the README for examples.

`(-> X f1 f2 ...)` threads X as the **last** argument of each form (last-arg, like Clojure's `->>`). Last-arg fits this dialect because the stateful/transform ops (`round`, `quantize`, `moving-*`, `squelch`, `deadband`) all take the value last. Gates (`squelch`, `deadband`) pass the value through on success and return `nil` on suppress. `publish-to` is a no-op on `nil`, which is what makes `(-> payload-float (round 1) (squelch) (publish-to (subject-append "stable")))` read top-to-bottom as "round, dedupe, emit."

## Outbound NATS bridge (`src/bridge.zig`)

Optional. Forwards local publishes whose subject matches one of the configured `:export` filters to a remote NATS cluster, via vendored nats.c. **Export-only** — nothing flows back. Configured via a single `(bridge :servers [...] :export [...] ...)` top-level form in the patchbay file (see `patchbay.edn` for the full keyword list).

Build: `-Dbridge=true` (default on, forced off for `zig build dist` because cross-compiling against OpenSSL for musl is out of scope). Dyn-links system OpenSSL via pkg-config (`brew install openssl@3` on macOS, `libssl-dev` on Linux). nats.c itself is vendored under `vendor/nats.c/src/`.

Why hand-written bindings (`src/nats_c.zig`) rather than `@cImport(nats.h)`: Zig 0.16.0's translate-c choked on the nats.h chain — same issue hit in `~/Experiments/zigxll-nats`, where the fix was also hand-writing the small subset we actually call.

Integration is a single hook: `Router` has a nullable `bridge_fn` that fan-out calls once per publish after local delivery. The bridge does its own subject-filter match internally (not via the subscription table). `nats.c` runs its own socket threads, but its `natsConnection_Publish` is thread-safe and non-blocking (buffers internally), so we can call it straight from the loop thread. No xev.Async thread-hop needed because we don't subscribe on the remote.

Counters published on the `$STATS.*` tick: `$STATS.bridge.published` and `$STATS.bridge.dropped`. Dropped includes connection-closed, long-subject-rejection (>= 512 bytes), and any other `natsConnection_Publish` failure.

Vendored nats.c layout: we keep `src/`, `unix/`, `glib/`; we drop `stan/`, `win/`, `adapters/`. The top-level nats.c sources for js/jsm/kv/object/micro are compiled (conn.c / sub.c / dispatch.c reference their symbols) but the APIs aren't exposed from Zig. STAN is off because `NATS_HAS_STREAMING` is never defined — its headers would need `protobuf-c/protobuf-c.h`, which we don't want to pull in.

## NATS protocol scope

Core only: `CONNECT`, `PUB`, `SUB`, `UNSUB`, `MSG`, `PING`, `PONG`, `INFO`, `+OK`, `-ERR`. No auth, TLS, queue groups, headers, JetStream, `$SYS.*` request-reply. `CONNECT` body is accepted and ignored. `+OK` is never sent (the spec makes it conditional on client `verbose: true`; `nats` CLI rejects `+OK` after CONNECT in the default non-verbose mode).

## Gotchas

- `std.fs.cwd()` does not exist in 0.16. Use `std.Io.Dir.cwd()` + `openFile(io, path, .{})`.
- `std.posix` is heavily stripped in 0.16 — `close`, `open`, `fstat` etc. are gone. If you need file I/O go through `std.Io.File` via `init.io`.
- The smoke test `scripts/smoke.sh` uses BSD `nc` flags (`-w` for timeout). Works on macOS; may need tweaks on Linux depending on which netcat flavor is installed.
- Debug builds are ~10× slower than release. Always bench with `--release=fast`.
- `zig version` must be exactly 0.16.0; the code uses `std.Io` API shapes that shift between Zig releases.
