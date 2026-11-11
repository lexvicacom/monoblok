description: Install monoblok from a binary, container or source, then run, deploy and size it.

# Install and run

{lede} monoblok ships as a single binary for Linux (x64 or ARM64) or macOS on Apple Silicon, and as a multi-arch container image. There is no cluster to form, and the only disk it uses is an optional [snapshot](state.html#snapshots) file for warm start.

## Install

### Binary

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash
```

The [release helper](https://github.com/lexvicacom/monoblok/blob/main/scripts/start.sh) downloads the latest monoblok (macOS/Linux) and extracts it into the current directory. To try it out, run the unpacked binary in place:

```sh
./monoblok-*/monoblok --port 14222 --patchbay ./monoblok-*/patchbay.edn
```

To install it on Linux, run the bundled installer and start the [systemd](#systemd) service:

```sh
sudo bash ./monoblok-*/install-systemd.sh
sudo systemctl enable --now monoblok
```

Release binaries require the OpenSSL 3 runtime libraries. On macOS, install them with `brew install openssl@3` before running monoblok. On Linux, install your distribution's OpenSSL 3 runtime package. Container images include these libraries.

The unpacked directory contains the runnable [examples](examples.html). Run the `.sh` files.

### Container

```sh
docker run --rm -p 14222:14222 ghcr.io/lexvicacom/monoblok:latest --port 14222
```

### Build from source

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/monoblok --port 14222 --patchbay patchbay.edn
```

Requires CMake 3.22+, a compiler with C23 mode (GCC 11+ or current Apple Clang), and system OpenSSL (1.1.1 or 3.x; 4.x is not supported). libuv, nats.c and yyjson are vendored.

## Command line

A patchbay file is the only configuration. Pass it positionally or with `--patchbay`; `.edn`, `.json`, `.yml` and `.yaml` are accepted.

```sh
monoblok --port 4222 --patchbay patchbay.edn
monoblok examples/sensors.edn
```

| option | meaning |
|---|---|
| `--patchbay FILE` | patchbay path (also accepted as the positional argument) |
| `--host HOST` / `--port PORT` | NATS listener, default `127.0.0.1:4222` |
| `--http-host HOST` / `--http-port PORT` | enable the [HTTP/SSE adapter](clients.html#http-sse-adapter) on this port |
| `--tls-cert FILE` / `--tls-key FILE` | enable [client TLS](clients.html#tls-for-local-nats-clients) |
| `--auth-token-env ENV` | require a CONNECT `auth_token` matching the value of `ENV` |
| `--auth-user-env ENV` / `--auth-pass-env ENV` | require a CONNECT user and password ([auth](clients.html#simple-auth-for-local-nats-clients)) |
| `--no-lvc` | disable the [`$LVC.*`](state.html) last-value cache |
| `--snapshot FILE` | load and write an LVC and patchbay state [snapshot](state.html#snapshots) |
| `--snapshot-every S` | write the snapshot every S seconds |
| `--stats-tick-ms MS` | [`$STATS.*`](state.html#stats-live-counters) publish cadence, default 60000 |
| `--validate` | parse and lint the patchbay, then exit |
| `--soundcheck` | read `SUBJECT|payload` rows from stdin and print what the rules emit |
| `--soundcheck-label` | prefix soundcheck rows with `in|` or `out|` |
| `--soundcheck-linger-ms MS` | keep clock and window state alive after soundcheck EOF, default 10000; 0 disables |
| `--trace` | log parsed client operations to stderr |
| `--io-uring` / `--no-io-uring` | opt in to libuv's io_uring paths on Linux (off by default, for seccomp-friendly containers) |
| `--version`, `-V` / `--help`, `-h` | print the version or usage |

`--validate`, `--soundcheck` and `--trace` are covered in [Validate and debug rules](patchbay.html#validate-and-debug-rules).

{{section docs/overview.md "Deploying" level=2}}

Restart behaviour depends on [snapshots](state.html#snapshots): with `--snapshot`, the last-value cache and the state inside gates and windows survive a restart.

{{section docs/overview.md "Design" level=2}}

## Running the tests

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target smoke
```

`smoke` runs the TCP server smoke, patchbay `soundcheck`, `load-smoke`, and the larger `load-soak` profile. `load-smoke` starts a temporary daemon and verifies exact TCP fan-out plus derived `moving-avg`, `moving-sum`, and `count!` streams.

{{section docs/overview.md "How fast is it?" level=2}}

The [saved benchmark runs](https://github.com/lexvicacom/monoblok/tree/main/bench-results) span up to 2-18 million msgs/sec across a 2-core ARM VPS, an 8-core x86_64 VPS, and an Apple Silicon M4 Mac mini for simple publish and fan-out workloads. Treat those numbers as directional samples and trends, not capacity promises.

Patchbay overhead scales with matching rules per PUB, not total rules in the file. On Linux the benchmark scripts default to monoblok's opt-in io_uring path to match the saved runs; pass `--epoll` to benchmark the production-default epoll path.

```sh
scripts/bench.sh
scripts/bench-with-nats-server.sh
```
