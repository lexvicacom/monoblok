# Monoblok C Spike

Experimental C23/libuv spike for the smallest Monoblok-shaped NATS server:

- accepts TCP clients
- parses `CONNECT`, `PING`, `SUB`, and `PUB`
- fans out literal-subject publishes to subscribers
- serializes writes through one guarded `uv_write_t` per connection

It also contains a standalone C port of the patchbay s-expression parser:

- arena-owned parse result
- lists `(...)`, vectors `[...]`, strings, symbols, keywords, booleans, nil,
  and numbers
- lists are always call forms: they must be non-empty and headed by a symbol;
  use vectors for sequence data
- `pb-dump` helper for inspecting parsed forms

The C patchbay slice also includes a small pure evaluator for early forms:
bound symbols, `do`, `if`, `when`, `not`, comparisons, arithmetic,
`and`, `or`, `->`, `str-concat`, `contains?`, affix checks, numeric helpers,
subject helpers, simple stateful gates/counters, `json-get`, `json-demux!`,
and `publish!` via callback. `monoblok-c --soundcheck PATCHBAY` runs that
subset over stdin rows shaped as `SUBJECT|payload`.

Language model for this spike:

- `(...)` is a call form. The head is always a symbol.
- `[...]` is data. Vector elements evaluate and return a vector.
- JSON patchbay files use arrays as call forms and `{"vec":[...]}` as vector
  data. Plain objects in form arguments expand to keyword/value pairs for
  config-shaped forms.
- Special forms are named explicitly (`if`, `when`, `do`) and are the only
  lazy forms.
- Side effects go through callbacks (`publish!`); pure forms stay ordinary
  value-returning calls.
- Do not add quote/macro/list-as-data compatibility paths. Prefer one obvious
  way to write each thing.

This is not feature parity with the Zig daemon. It intentionally omits full
patchbay parity, LVC, snapshots, mixer, bridge, `UNSUB`, queue groups, wildcard
subjects, headers, auth, and TLS.

## Dependencies

The preferred layout is vendored:

```sh
git clone https://github.com/libuv/libuv.git c-spike/vendor/libuv
git clone https://github.com/ibireme/yyjson.git c-spike/vendor/yyjson
```

If `vendor/libuv` is absent, CMake falls back to system libuv through
`pkg-config`. yyjson is used as vendored source for JSON patchbay loading and
JSON payload operations.

## Commands

```sh
cmake -S c-spike -B c-spike/build -DCMAKE_BUILD_TYPE=Release
cmake --build c-spike/build
ctest --test-dir c-spike/build --output-on-failure
cmake --build c-spike/build --target smoke
./c-spike/build/pb-dump patchbay.edn
./c-spike/build/monoblok-c --version
./c-spike/build/monoblok-c --validate c-spike/examples/strict-vectors.edn
./c-spike/build/monoblok-c --validate c-spike/examples/strict-vectors.json
printf 'sensors.temp|31\n' | ./c-spike/build/monoblok-c --soundcheck c-spike/examples/strict-vectors.edn
printf 'sensors.temp|{"temp":31,"status":"warm"}\n' | ./c-spike/build/monoblok-c --soundcheck c-spike/examples/strict-vectors.json
```

Sanitizers:

```sh
cmake -S c-spike -B c-spike/build-asan -DMB_ASAN=ON
cmake --build c-spike/build-asan
ctest --test-dir c-spike/build-asan --output-on-failure
```

Run manually:

```sh
./c-spike/build/monoblok-c --host 127.0.0.1 --port 4222
```
