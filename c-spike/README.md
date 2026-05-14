# Monoblok C Spike

C23/libuv port spike. Build with CMake from the repository root.

## Configure

```sh
cmake -S c-spike -B c-spike/build
cmake -S c-spike -B c-spike/build-release -DCMAKE_BUILD_TYPE=Release
cmake -S c-spike -B c-spike/build-asan -DMB_ASAN=ON
```

Useful cache variables:

- `CMAKE_BUILD_TYPE=Debug|Release|RelWithDebInfo|MinSizeRel`
- `MB_ASAN=ON` enables AddressSanitizer and UBSan.
- `MONOBLOK_C_VERSION=...` sets the version string compiled into `monoblok-c`.

`vendor/libuv` is used when present. Otherwise CMake looks for system libuv via
`pkg-config`. `vendor/yyjson` is built directly.

## CMake Targets

Primary:

```sh
cmake --build c-spike/build                  # default build
cmake --build c-spike/build --target monoblok-c
cmake --build c-spike/build --target bench-patchbay
cmake --build c-spike/build --target pb-dump
```

Tests:

```sh
cmake --build c-spike/build --target unit-tests
cmake --build c-spike/build --target sexpr-tests
cmake --build c-spike/build --target eval-tests
cmake --build c-spike/build --target json-tests
cmake --build c-spike/build --target program-tests
ctest --test-dir c-spike/build --output-on-failure
```

Smoke and checks:

```sh
cmake --build c-spike/build --target smoke
cmake --build c-spike/build --target soundcheck
cmake --build c-spike/build --target tidy       # if clang-tidy was found
```

Libraries:

```sh
cmake --build c-spike/build --target monoblok_core
cmake --build c-spike/build --target patchbay_sexpr
cmake --build c-spike/build --target yyjson
cmake --build c-spike/build --target uv_a
```

List generated targets:

```sh
cmake --build c-spike/build --target help
```

## Run

```sh
./c-spike/build/monoblok-c --help
./c-spike/build/monoblok-c --version
./c-spike/build/monoblok-c --host 127.0.0.1 --port 4222
./c-spike/build/monoblok-c --host 127.0.0.1 --port 4222 --no-lvc
./c-spike/build/monoblok-c --host 127.0.0.1 --port 4222 --patchbay patchbay.edn
./c-spike/build/monoblok-c --validate patchbay.edn
./c-spike/build/monoblok-c --soundcheck patchbay.edn
```

## Patchbay Helpers

```sh
./c-spike/build/pb-dump patchbay.edn
printf 'sensors.temp|31\n' | ./c-spike/build/monoblok-c --soundcheck patchbay.edn
```

## Benchmarks

Patchbay evaluator microbench:

```sh
cmake --build c-spike/build-release --target bench-patchbay
./c-spike/build-release/bench-patchbay pass 1 1000000
./c-spike/build-release/bench-patchbay gate 1 1000000
./c-spike/build-release/bench-patchbay window 1 1000000
./c-spike/build-release/bench-patchbay json 1 1000000
./c-spike/build-release/bench-patchbay mixed 1 1000000
```

Modes match the Zig bench shape:

- `pass`: dispatch plus `publish!`
- `gate`: `payload-float`, `squelch`, `publish!`
- `window`: `payload-float`, `moving-avg`, `publish!`
- `json`: `json-demux!` over a tiny object
- `mixed`: load `./patchbay.edn`

NATS CLI comparison script:

```sh
NATS_URL=nats://127.0.0.1:42230 BENCH_COOLDOWN_S=0 \
  bash c-spike/scripts/bench-with-nats-server.sh
```
