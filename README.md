# monoblok

A NATS-core compatible messaging system that conditions subjects before subscribers see them.

> Fix raw input streams once, not in every subscriber.
>
> Put monoblok in front of your NATS clients, declare the useful subjects you actually wanted, and let subscribers consume clean streams instead of raw noise.

## Rationale

It is not uncommon for systems to contain some _caretaker_ services that subscribe to ingress NATS subjects to clean up and republish a raw stream before the real business starts. This might include rounding, dedup, deadband, JSON demux, OHLC bars, threshold alerts and so on. High velocity or miniscule changes don't always have value downstream. monoblok lets you declare that tidying work once, leveraging efficient implementations of common tasks as rules at the broker, instead of writing _rounding logic_ N times in N services.

**Declare it once, as rules, in the broker.**

monoblok speaks NATS. Point your NATS clients at it and the conditioning happens on the way through. Rules live in patchbay, a small S-expression DSL.

![monoblok round and squelch demo](./docs/monoblok-round-squelch-fixed.gif)

Common ways of running monoblok:
- Standalone broker: clients connect directly to monoblok for lightweight NATS-core pub/sub with signal conditioning built in.
- Signal conditioning front door: publishers send raw events to monoblok, monoblok cleans them, then forwards selected subjects to a real NATS cluster.

![monoblok deployment modes](./docs/infographic.png)

monoblok is written in C with libuv and builds on Linux and macOS. It aims to be simple, lightweight and **fast**, even on entry level/shared hardware. Smoke tests and benchmarks are part of the build; the [saved results](./bench-results) show low millions of msgs/sec on a 2-core ARM VPS, so monoblok is unlikely to be the bottleneck for many likely conditioning workloads. Treat the numbers as directional rather than scientific.

[tinyblok](https://github.com/lexvicacom/tinyblok) is an implementation of the same idea, but for microcontrollers.

See [Overview](./docs/overview.md), [Patchbay](./docs/patchbay.md), and the runnable files in [examples/](./examples/) to better get a feel. Also, there's [the introductory blog post](https://alexjreid.dev/posts/monoblok/) [and friends](https://alexjreid.dev/tags/monoblok/).

## Install

The [release helper](./scripts/start.sh) downloads monoblok (macOS/Linux) into the current directory:

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash
```

Then run the unpacked binary:

```sh
./monoblok-*/monoblok --port 14222 --patchbay ./monoblok-*/patchbay.edn
```

To add as a service on systemd Linux, the release tarball includes `install-systemd.sh`.

## Container

Pull the latest container image (arm64, x86_64 multi arch):

```sh
docker pull ghcr.io/lexvicacom/monoblok:latest
```

Run monoblok from the image:

```sh
docker run --rm -p 14222:14222 ghcr.io/lexvicacom/monoblok:latest --port 14222
```

## Build locally

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/monoblok --port 14222 --patchbay patchbay.edn
```

Compiles cleanly on macOS and Linux. Dependencies are vendored.

## AI

It's 2026, Claude and Codex help me a lot. All code is reviewed and iterated upon before being merged.

#### Didn't this used to be written in Zig?

It did, but the good parts of Zig didn't justify its use in **this project**. A project like monoblok ought to be boring technically — a solid, tiny, trusted tool that just gets out of the way.

I love the idea of Zig, but I know C better. I felt uneasy not being able to explain some of the tricky corners involving `@ptrCast`, `anytype`, `inline`, and `std.Io` that an LLM had generated. It's also a shifting target, which hurts. With a good prompt to force 0.16 semantics, Zig is fine with coding assistants, but that's churn and effort spent elsewhere.

You could say that many of Zig's virtues come from being forced to think low-level: explicit allocators threaded through your code, no hidden control flow, etc. Zig's community is also fairly anti-AI, which is their call but adds friction for how I want to work. Vibe coding/AI slop is a problem, bug one shouldn't tar with the same brush.

This project is better in C. It has a small surface area an LLM can understand and spot flaws in, given its knowledge of an ancient target (C17). Combined with mature tooling: ASan, UBSan — this is an acceptable trade IMHO. I like the [Redis style of C](https://github.com/antirez/redis/blob/unstable/MANIFESTO), where you write a minimal domain-specific "not quite DSL" to use, without blurring actual functionality in frameworky BS or macro soup.

Simple C code makes the codebase a breeze to work on by hand. _This is just, like, my opinion, man._

## License

MIT. See `LICENSE`.
