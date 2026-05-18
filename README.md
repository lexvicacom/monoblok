# monoblok

Signal conditioning, transformation, and routing in a NATS-native message processor.


  > **monoblok speaks NATS.** Point existing publishers at it or have it consume existing subjects; declare
  > the processing rules you need, and let subscribers consume clean, actionable
  > subjects.
  >
  > Fix noisy raw input streams once, not in every subscriber.

## Rationale

It is not uncommon for systems to contain some _caretaker_ services that subscribe to ingress NATS subjects to clean up and republish a raw stream before the real business starts. This might include rounding, dedup, deadband, JSON demux, OHLC bars, threshold alerts and so on. High velocity or miniscule changes don't always have value downstream. monoblok lets you declare that tidying work once, leveraging efficient implementations of common tasks as rules at the broker, instead of writing _rounding logic_ N times in N services.

**Declare it once, as rules, at the edge.**

![monoblok round and squelch demo](./docs/monoblok-round-squelch-fixed.gif)


Rules live in [patchbay](./docs/patchbay.md), a small DSL which can be expressed as YAML, EDN or JSON. A walked example lives in [patchbay.edn](./patchbay.edn). You can also [write patchbay files as YAML](https://github.com/lexvicacom/monoblok/blob/main/examples/demo.yml). 

<a href="https://lexvicacom.github.io/monoblok/show-n-tell/moonwell_linkedin_demo.html" target="_blank" rel="noopener noreferrer">patchbay also lends itself well to help from coding assistants</a> when fed [AGENTS_PATCHBAY.md](https://github.com/lexvicacom/monoblok/blob/main/docs/AGENTS_PATCHBAY.md) (to be honest the agent instructions are human-parseable if you prefer a succinct primer. There is a fuller [patchbay guide](https://github.com/lexvicacom/monoblok/blob/main/docs/patchbay.md) and  [cheatsheet](https://github.com/lexvicacom/monoblok/blob/main/docs/patchbay-cheatsheet.md).


#### Common ways of running monoblok:
- **Tap into existing NATS:** monoblok subscribes to selected subjects on your NATS environment, treats them as private patchbay input, then emits back only the cleaned or derived subjects your rules choose.
- **Signal conditioning front door:** publishers send raw events to monoblok, monoblok cleans them, then forwards selected subjects to a  NATS cluster.
- **Standalone broker:** NATS clients connect directly to monoblok for lightweight NATS-core pub/sub with signal conditioning built in.

### Tiny and fast
monoblok is written in C with libuv and builds on Linux and macOS. It aims to be simple, lightweight and **fast**, even on entry level/shared hardware. Smoke tests and load checks are part of the build; dedicated benchmark helpers live in [scripts/](./scripts). The [saved benchmark runs](./bench-results) span up to **2-18 million msgs/sec** across a 2-core ARM VPS, an 8-core x86_64 VPS, and an Apple Silicon M4 Mac mini for simple publish and fan-out workloads. Treat those numbers as directional samples/trends and not capacity promises in the real world. See [running tests](#running-tests) for tests that exercise the router and parser without network.

### Read more
[tinyblok](https://github.com/lexvicacom/tinyblok) is an implementation for microcontrollers relaying cleaned sensor data into NATS.

See [Overview](./docs/overview.md), [Patchbay](./docs/patchbay.md), and the runnable files in [examples/](./examples/) to better get a feel. Also, there's [the introductory blog post](https://alexjreid.dev/posts/monoblok/) [and friends](https://alexjreid.dev/tags/monoblok/).

![monoblok deployment modes](./docs/infographic.png)

## Run it

### Binary

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash
```
The [release helper](./scripts/start.sh) downloads the latest monoblok (macOS/Linux) and extracts it into the current directory. To run the unpacked binary:

```sh
./monoblok-*/monoblok --port 14222 --patchbay ./monoblok-*/patchbay.edn
```

The directory contains runnable examples. Run the `.sh` files.

To add as a service on systemd Linux, run `scripts/install-systemd.sh`.

### Container

Multi-arch image:

```sh
docker run --rm -p 14222:14222 ghcr.io/lexvicacom/monoblok:latest --port 14222
```

### Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/monoblok --port 14222 --patchbay patchbay.edn
```

Compiles cleanly on macOS and Linux. Dependencies are vendored. System `openssl` required.

## Running tests

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Fast integration smoke:

```sh
cmake --build build --target smoke
```

`smoke` runs the TCP server smoke, patchbay `soundcheck`, `load-smoke`, and
the larger `load-soak` profile.
The subchecks can also be run directly:

```sh
cmake --build build --target soundcheck
cmake --build build --target load-smoke
cmake --build build --target load-soak
```

`load-smoke` starts a temporary daemon and verifies exact TCP fan-out plus
derived `moving-avg`, `moving-sum`, and `count!` streams. `load-soak` runs the
same check with a heavier subscriber/message profile.

Benchmark helpers are separate from the test targets because they depend on the
NATS CLI, and the comparison script uses `nats-server` when available:

```sh
scripts/bench.sh
scripts/bench-with-nats-server.sh
```

Saved sample output lives in [bench-results/](./bench-results). On Linux these
scripts default to monoblok's opt-in libuv io_uring path to match the saved
runs; pass `--epoll` to benchmark the production-default epoll path.

## Support

I've been doing this for a while - while the same-old problems monoblok solves may not be readily apparent - _"it's simple - it is just rounding numbers and doing basic stats"_ ... those with battle scars are hopefully nodding along. [My company](https://lexvica.com) can provide services around monoblok. I'd be happy to learn about your environment, requirements and work with you on a proof of concept, case study or complete solution. [Drop me a line](mailto:alex@lexvica.com).

## AI

It's 2026, Claude and Codex help a lot. All code is reviewed and iterated upon before being merged.

>Personal note: I had a stroke in Dec 2025 and have oddly adapted to typing with one finger with my left hand. I'd probably have given up without these tools, during my recovery. Five months on my typing has got better but it is still error prone. Think this may be as good as it gets!

## License

MIT. See `LICENSE`.
