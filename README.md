# monoblok Overview

> A NATS-core compatible messaging system that conditions subjects before subscribers see them.
> 
> Fix raw input streams once, not in every subscriber.

## Rationale

It is not uncommon for systems to contain some _caretaker_ services that subscribe to ingress NATS subjects to clean up and republish a raw stream before the real business starts. This might include rounding, dedup, deadband, JSON demux, OHLC bars, threshold alerts and so on. High velocity or miniscule changes don't always have value downstream. monoblok lets you declare that tidying work once, leveraging efficient implementations of common tasks as rules at the broker, instead of writing _rounding logic_ N times in N services.

**Declare it once, as rules, in the broker.**

monoblok speaks NATS. Point your NATS clients at it and the conditioning happens on the way through. Rules live in patchbay, a small S-expression DSL.

![monoblok round and squelch demo](./docs/monoblok-round-squelch-fixed.gif)

Common ways of running monoblok:
- Standalone broker: clients connect directly to monoblok for lightweight NATS-core pub/sub with signal conditioning built in.
- Signal conditioning front door: publishers send raw events to monoblok, monoblok cleans them, then forwards selected subjects to a real NATS cluster.

![monoblok deployment modes](./docs/infographic.png)

monoblok is written in C with libuv and builds on Linux and macOS. It aims to be **fast**, even on entry level/shared hardware. There are no scientific measurements yet. There are some [benchmark scripts](../scripts) and [results](../bench-results).

[tinyblok](https://github.com/lexvicacom/tinyblok) is an implementation of the same idea, but for microcontrollers.

[Read the introductory blog post](https://alexjreid.dev/posts/monoblok/) [and friends](https://alexjreid.dev/tags/monoblok/).

For a user-facing overview, deployment examples, and DSL introduction, see [docs/overview.md](./docs/overview.md),
[docs/patchbay.md](./docs/patchbay.md), and the runnable files in [examples/](./examples/).

## Install

The release helper downloads a platform tarball into the current directory:

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash
```

Then run the unpacked binary:

```sh
./monoblok-*/monoblok --port 4222 --patchbay ./monoblok-*/patchbay.edn
```

For Linux services, the release tarball includes `install-systemd.sh`.

### Docker (arm64, x86_64)

```sh
docker run --rm -p 4222:4222 ghcr.io/lexvicacom/monoblok:latest
```

This will start monoblok on port 4222. You can use the NATS CLI to pub and sub to it.

The image includes the root [`patchbay.edn`](../patchbay.edn) as `/etc/monoblok/patchbay.edn` as a starting point. Read this file for guidance on where to publish and what to expect. 

`monoblok` is the entrypoint so you can pass any parameters such as --port after the image name. 

See the [Container](#container) section for info on use with orchestrators, ECR etc.
