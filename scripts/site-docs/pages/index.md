description: What monoblok is, the three ways to run it, and a five-minute first rule.

# monoblok documentation

{lede} monoblok is a small NATS-compatible message processor. It rounds, dedupes, smooths, demuxes and routes messages in flight, using rules written in a small language called patchbay, so subscribers do not have to.

## The problem it solves

Sensor and market data feeds often publish more frequently, or at greater precision, than subscribers need. Each subscriber then implements similar rounding, filtering and deduplication logic.

A separate processing service can republish these feeds, but it also needs deployment and maintenance.

monoblok applies patchbay rules before fan-out. For example, this rule rounds sensor readings and suppresses duplicates:

```edn
; Round to 1dp, drop duplicates, emit on the .stable sub-subject.
(on "sensors.*"
  (-> payload-float
      (round 1)
      (squelch)
      (publish! (subject-append "stable"))))
```

Subscribers that want the conditioned value listen to `sensors.*.stable`. The raw stream stays available for anyone who needs it. Here is that rule fed seven jittery readings:

```soundcheck examples/sensors.edn
sensors.temp|42.01
sensors.temp|42.04
sensors.temp|42.08
sensors.temp|42.12
sensors.temp|43.00
sensors.temp|43.00
sensors.temp|43.01
```

## Three ways to run it

monoblok speaks the NATS protocol, so existing clients connect without changes.

- **Tap into existing NATS.** monoblok subscribes to selected subjects on your NATS environment, treats them as private patchbay input, then emits back only the cleaned or derived subjects your rules choose. See [import](bridge.html#import).
- **Signal conditioning front door.** Publishers send raw events to monoblok, monoblok cleans them, then forwards selected subjects to a NATS cluster. See [export](bridge.html#export).
- **Standalone broker.** NATS clients connect directly to monoblok for lightweight NATS-core pub/sub with signal conditioning built in.

For the front door pattern, change the publishers' server address to monoblok and configure an export to the cluster. Subscribers continue to connect to the cluster and use the processed subjects.

## Five-minute start

Install the binary (macOS or Linux), or use the container image. [Install and run](running.html) has the details.

```sh
curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash
./monoblok-*/monoblok --port 14222 --patchbay ./monoblok-*/patchbay.edn
```

With the [NATS CLI](https://github.com/nats-io/natscli) in two more terminals, subscribe and publish:

```sh
nats -s nats://127.0.0.1:14222 sub 'sensors.>'
nats -s nats://127.0.0.1:14222 pub sensors.temp 21.04
nats -s nats://127.0.0.1:14222 pub sensors.temp 21.01
nats -s nats://127.0.0.1:14222 pub sensors.temp 21.42
```

You can try rules without a server or a NATS client at all. `--soundcheck` reads `SUBJECT|payload` rows on stdin and prints what the rules publish:

```sh
printf 'sensors.temp|31\n' | monoblok --soundcheck examples/sensors.edn
```

A [public demo server](https://github.com/lexvicacom/monoblok/blob/main/docs/demo.md) runs on `demo.monoblok.host:4222`, and the [live demo](../demo.html) page connects to it without a local install.

## Where to go next

<ul class="cards">
<li><a href="patchbay.html"><strong>Patchbay guide</strong><span>The rule language: syntax, threading, state, and the EDN, YAML and JSON spellings.</span></a></li>
<li><a href="forms.html"><strong>Forms reference</strong><span>Every form with its arguments, behaviour and a worked snippet.</span></a></li>
<li><a href="examples.html"><strong>Examples</strong><span>Runnable patchbays from a five-line sensor rule to a market data firehose.</span></a></li>
<li><a href="state.html"><strong>LVC, stats and snapshots</strong><span>Last-value streams for late joiners, live counters, and warm restarts.</span></a></li>
<li><a href="clients.html"><strong>NATS clients, auth and HTTP</strong><span>Protocol coverage, tokens, TLS, and the HTTP/SSE adapter for browsers.</span></a></li>
<li><a href="bridge.html"><strong>Export, import and JetStream</strong><span>Pairing monoblok with a real NATS cluster, including JetStream replay.</span></a></li>
</ul>

## Why put this at the broker

Processing before fan-out avoids repeating the same operations in every subscriber. A patchbay rule can round temperatures, suppress GPS jitter or filter price changes, and publish the result on a subject shared by all consumers.

The last-value cache serves a different purpose: a new subscriber can receive the most recent value immediately rather than waiting for the next event. See [LVC, stats and snapshots](state.html) for cache configuration and restart behaviour.

## When it is not the right tool

monoblok is a core NATS server with rules, not a replacement for a cluster. It has no JetStream storage, no clustering and no message headers on its own listener. It can consume JetStream messages and forward selected subjects to a NATS cluster.

If plain publish and subscribe is all you need, nats-server is the better choice.

## More reading

- [The introductory blog post](https://alexjreid.dev/posts/monoblok/) covers where the idea came from, and there are [more posts about monoblok](https://alexjreid.dev/tags/monoblok/) on the blog.
- [tinyblok](tinyblok.html) runs a subset of patchbay on ESP32 boards, relaying cleaned sensor data into NATS.
- The Markdown docs these pages are built from live in the [repository](https://github.com/lexvicacom/monoblok/tree/main/docs).
