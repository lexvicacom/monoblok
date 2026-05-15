# AGENTS.md

Guidance for coding agents working in this C23/libuv Monoblok tree.

## Shape

Monoblok is a compact C23 NATS-like daemon with a patchbay routing DSL,
optional LVC, snapshots, JSON helpers, and an export-only NATS bridge through
vendored `nats.c`.

Keep the layout shallow:

- `src/`: core pieces (`buf`, `proto`, `router`, `snapshot`, `bridge`, `main`).
- `src/server/`: libuv listener, connection lifetime, read/write callbacks.
- `src/patchbay/`: arena, parser, JSON adapter, evaluator, validation,
  soundcheck, and patchbay helpers.
- `vendor/libuv/`: vendored libuv source.
- `vendor/nats.c/`: vendored NATS C client.
- `vendor/yyjson/`: pruned yyjson source, license, and readme.
- `test/`: unit tests plus script-driven smoke/soundcheck checks.
- `examples/`: runnable patchbay examples.
- `docs/`: user-facing overview, patchbay docs, and documentation images.

Prefer local, explicit C over frameworks or abstraction layers. The point of
this branch is proving plain C can stay readable while staying fast.

## Build And Test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target smoke
cmake --build build --target soundcheck
scripts/bridge-smoke.sh
```

Sanitizer pass:

```sh
cmake -S . -B build-asan -DMB_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Useful targets:

```sh
cmake --build build --target monoblok
cmake --build build --target bench-patchbay
cmake --build build --target pb-dump
```

## Core Invariants

- The server runs on one libuv loop thread. Do not add hot-path mutexes or
  atomics unless the threading model changes.
- Keep one guarded `uv_write_t` per connection. Queue bytes in buffers, swap
  into `in_flight`, and resubmit only after completion.
- Router owns subscriptions, LVC, bridge callback hooks, and output fanout only.
  It must not know about libuv handles.
- Server owns connection lifetime and write completions.
- Protocol parsing is slice-based and allocation-free.
- `mb_slice`/`pb_slice` values are borrowed views unless a helper explicitly
  duplicates them. Do not store a slice beyond the lifetime of its source
  buffer or arena.
- Patchbay parse trees and temporary eval values live in arenas.
- Long-lived patchbay state is owned by `pb_eval_state`; be explicit about
  string/ring ownership and free every heap field in `state_entry_free`.
- Avoid surprise publish-time allocation. Allocate on connection open, SUB,
  UNSUB, patchbay/config load, bridge startup, or first state-slot creation;
  reuse read/write buffers and per-publish scratch arenas.
- On Linux, libuv uses epoll for the event loop. The daemon disables libuv's
  optional io_uring paths by default for seccomp-friendly containers; use
  `--io-uring` or `UV_USE_IO_URING=1` to opt in before loop creation.

## Patchbay Model

- Lists are call forms. A list must be non-empty and headed by a symbol.
- Vectors are data.
- JSON patchbay files use arrays as call forms and `{"vec":[...]}` for vector
  data. Plain objects in form arguments expand to keyword/value pairs.
- Do not add quote, macro, or list-as-data compatibility paths.
- Keep effectful forms explicit, conventionally with `!` (`publish!`,
  `json-demux!`).
- If an evaluator form is not implemented, validation should fail rather than
  silently accepting it.
- Patchbay messages re-enter rule evaluation only when the emitting rule is
  marked `:reentrant true`. Re-entry is capped to avoid infinite loops.
- Negative numeric literals and negative payloads are valid. When testing with
  `nats pub`, pass negative bodies after `--`, for example
  `nats pub sensors.temp -- -5`.

## LVC And Snapshots

`$LVC.<subject>` is a live last-value stream. Subscribing registers a normal sub
against the stripped inner subject with an `is_lvc` flag, and immediately emits
matching cached values. Publishing to `$LVC.*` is rejected. `--no-lvc` disables
the cache and rejects `$LVC.*` subscribes.

Snapshots are optional warm-start persistence:

- `--snapshot PATH`: load at startup if present.
- `--snapshot-every SECONDS`: periodic dump.

Snapshots include LVC entries and patchbay state. Snapshot rule state identity
is `(rule_idx, filter)`. If the patchbay changed and the recorded filter no
longer matches, that rule state is skipped with a warning. LVC entries load
regardless.

## Bridge Mode

The outbound bridge is optional and export-only. It is configured by a
top-level `(bridge ...)` form in the patchbay file. Leaving the form out makes
runtime cost zero.

Bridge fan-out uses `Router.bridge_fn`, called once per publish after local
delivery. The bridge does its own subject-filter matching and publishes matching
subjects to the remote NATS cluster through `nats.c`, which owns reconnects and
outbound buffering.

## NATS Protocol Scope

Core only: `CONNECT`, `PUB`, `SUB`, `UNSUB`, `MSG`, `PING`, `PONG`, `INFO`,
`+OK`, `-ERR`.

There is no auth on the server side, queue groups, headers, JetStream, mixer,
or `$SYS.*` request-reply. `CONNECT` bodies are accepted and ignored. `+OK` is
never sent.

## C Style

- Use C23 where it reduces noise, but keep code portable under the configured
  compiler flags.
- The target and binary are named `monoblok`; avoid reintroducing `monoblok-c`
  in scripts, docs, or build targets.
- The root `.clang-format` intentionally uses `ColumnLimit: 0` to avoid
  save-on-format churn in generated-looking compact C. Do not run broad
  mechanical formatting unless explicitly requested.
- Keep every project-owned struct documented with one short comment explaining
  its role, ownership, or lifetime. This includes small view types such as
  slices.
- Prefer arena copies for AST text and evaluator-owned output text.
- Keep comments concise and focused on ownership, lifetime, invariants, or
  non-obvious protocol behavior.
- Do not let vendored code inherit project warning flags.
- Keep dependencies small and easy to audit.
- `pb_eval.c` contains evaluator dispatch and binding semantics. Keep large
  builtin bodies in `pb_builtins.c` or split them further before the evaluator
  gets hard to scan.

## Verification

For most source changes, run:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

For behavior touching I/O, routing, snapshots, or bridge, add the relevant
smoke test:

```sh
cmake --build build --target smoke
scripts/bridge-smoke.sh
```

Use a `Release` CMake build before reporting performance.
