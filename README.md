# monoblok

A NATS-core compatible messaging system that conditions subjects before subscribers see them.

> Fix raw input streams once, not in every subscriber.
>
> Put monoblok in front of your NATS clients, declare the useful subjects you actually wanted, and let subscribers consume clean streams instead of raw noise.

## Rationale

It is not uncommon for systems to contain some _caretaker_ services that subscribe to ingress NATS subjects to clean up and republish a raw stream before the real business starts. This might include rounding, dedup, deadband, JSON demux, OHLC bars, threshold alerts and so on. High velocity or miniscule changes don't always have value downstream. monoblok lets you declare that tidying work once, leveraging efficient implementations of common tasks as rules at the broker, instead of writing _rounding logic_ N times in N services.

**Declare it once, as rules, in the broker.**

monoblok speaks NATS. Point your NATS clients at it and the conditioning happens on the way through. Rules live in [patchbay](./docs/patchbay.md), a small S-expression DSL. It is easy to get started as [patchbay lends itself well to help from coding assistants](https://lexvicacom.github.io/monoblok/show-n-tell/moonwell_terminal_demo.html).

![monoblok round and squelch demo](./docs/monoblok-round-squelch-fixed.gif)

#### Common ways of running monoblok:
- Standalone broker: clients connect directly to monoblok for lightweight NATS-core pub/sub with signal conditioning built in.
- Signal conditioning front door: publishers send raw events to monoblok, monoblok cleans them, then forwards selected subjects to a real NATS cluster.

![monoblok deployment modes](./docs/infographic.png)

monoblok is written in C with libuv and builds on Linux and macOS. It aims to be simple, lightweight and **fast**, even on entry level/shared hardware. Smoke tests and benchmarks are part of the build; the [saved results](./bench-results) show low millions of msgs/sec on a 2-core ARM VPS, so monoblok is unlikely to be the bottleneck for many likely conditioning workloads. Treat the numbers as directional rather than scientific.

[tinyblok](https://github.com/lexvicacom/tinyblok) is an implementation of the same idea, but for microcontrollers.

See [Overview](./docs/overview.md), [Patchbay](./docs/patchbay.md), and the runnable files in [examples/](./examples/) to better get a feel. Also, there's [the introductory blog post](https://alexjreid.dev/posts/monoblok/) [and friends](https://alexjreid.dev/tags/monoblok/).

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash
```
The [release helper](./scripts/start.sh) downloads monoblok (macOS/Linux) and extracts it into the current directory. To run the unpacked binary:

```sh
./monoblok-*/monoblok --port 14222 --patchbay ./monoblok-*/patchbay.edn
```

The directory contains runnable examples. Run the `.sh` files.

To add as a service on systemd Linux, run `scripts/install-systemd.sh`.

## Container

Multi-arch image:

```sh
docker run --rm -p 14222:14222 ghcr.io/lexvicacom/monoblok:latest --port 14222
```

## Build locally

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/monoblok --port 14222 --patchbay patchbay.edn
```

Compiles cleanly on macOS and Linux. Dependencies are vendored. System `openssl` required.

## Still reading?

See [Overview](./docs/overview.md), [Patchbay](./docs/patchbay.md), and the runnable files in [examples/](./examples/) to better get a feel. Also, there's [the introductory blog post](https://alexjreid.dev/posts/monoblok/) [and friends](https://alexjreid.dev/tags/monoblok/).

## AI

It's 2026, Claude and Codex help me a lot. All code is reviewed and iterated upon before being merged.

>Personal note: I had a stroke in Dec 2026 and have oddly adapted to typing with one finger with my left hand. I'd probably have given up without these tools, during my recovery. Five months on my typing has got better but it is still error prone. Think this may be as good as it gets!

## License

MIT. See `LICENSE`.
