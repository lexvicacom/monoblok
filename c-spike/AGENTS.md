# AGENTS.md

Guidance for coding agents working in the C spike. The parent repository rules
still apply; this file only narrows them for `c-spike/`.

## Shape

This is an experimental C23/libuv port of the smallest useful Monoblok core.
Keep the layout shallow:

- `src/`: small core pieces (`buf`, `proto`, `router`, `main`).
- `src/server/`: libuv listener, connection lifetime, read/write callbacks.
- `src/patchbay/`: arena, strict parser, JSON adapter, evaluator, validation,
  soundcheck, and patchbay-only helpers.
- `vendor/libuv/`: vendored libuv source.
- `vendor/yyjson/`: pruned yyjson source, license, and readme.

Prefer local, explicit C over frameworks or abstraction layers. This spike is
supposed to answer whether plain C can stay readable while staying fast.

## Build And Test

```sh
cmake -S c-spike -B c-spike/build -DCMAKE_BUILD_TYPE=Release
cmake --build c-spike/build
ctest --test-dir c-spike/build --output-on-failure
cmake --build c-spike/build --target soundcheck
cmake --build c-spike/build --target smoke
```

Sanitizer pass:

```sh
cmake -S c-spike -B c-spike/build-asan -DMB_ASAN=ON
cmake --build c-spike/build-asan
ctest --test-dir c-spike/build-asan --output-on-failure
```

## Core Invariants

- The server runs on one libuv loop thread. Do not add hot-path mutexes or
  atomics unless the threading model changes.
- Keep one guarded `uv_write_t` per connection. Queue bytes in buffers, swap
  into `in_flight`, and resubmit only after completion.
- Router owns subscriptions and output fanout only. It must not know about
  libuv handles.
- Server owns connection lifetime and write completions.
- Protocol parsing is slice-based and allocation-free.
- Patchbay parse trees and temporary eval values live in arenas.

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

## C Style

- Use C23 where it reduces noise, but keep code portable under the configured
  compiler flags.
- Keep structs that carry important ownership or state commented with one short
  comment explaining their role.
- Prefer arena copies for AST text and evaluator-owned output text.
- Keep comments concise and focused on ownership, lifetime, invariants, or
  non-obvious protocol behavior.
- Do not let vendored code inherit project warning flags.
- Keep dependencies small and easy to audit.
