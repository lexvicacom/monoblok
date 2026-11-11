description: What monoblok is, the three ways to run it, and a five-minute first rule.

# monoblok documentation

{lede} monoblok is a small NATS-compatible message processor. It rounds, dedupes, smooths, demuxes and routes messages in flight, using rules written in a small language called patchbay, so subscribers do not have to.

## The problem it solves

A car publishes its engine RPM ten times a second. A market data feed ticks on every quote. A cheap temperature sensor posts a fresh reading every two seconds, mostly identical to the last one. Data moves quickly, but most of it is noise: a publisher that does not know what its subscribers care about, and a fleet of subscribers each writing the same defensive code to round, debounce, and ignore the boring readings.

Many systems grow small *caretaker* services to deal with this. Each one subscribes to a raw subject, tidies it and republishes it. They work, but each is another program to write, deploy and keep in step with the others.

monoblok does that work once, before a message reaches any subscriber. The cleanup logic is declared once, as rules, instead of being re-implemented in every consumer:

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

The front door pattern needs no code changes: publishers PUB to monoblok instead of directly to NATS, using the exact same NATS client. monoblok does the conditioning, then forwards the tidy subjects to your real cluster. Consumers of the processed subjects are none the wiser; they connect to the same production NATS environment as before.

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

No install at all: a [public demo server](https://github.com/lexvicacom/monoblok/blob/main/docs/demo.md) runs on `demo.monoblok.host:4222`, and the [live demo](../demo.html) page drives it from the browser.

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

The conventional logic is "broker moves bytes, application does logic." That is fine and largely correct, but there is a category of logic, signal conditioning, that you could argue belongs at the broker. It is stateless from the application's point of view, it is the same boring code reimplemented in every consumer, and it benefits enormously from being applied once, centrally, before fan-out.

Putting a small DSL at the broker for this kind of work is a nice middle ground. It is not trying to be Flink, Beam or Kafka Streams. It is a few primitives, declared once, that turn raw sensor noise into something useful before it ever leaves the broker. The last-value cache then makes late-joining subscribers a non-event, which is the other thing every realtime app ends up reinventing.

Swap office temperature sensors for market data ticks where you want to deadband out the noise and only emit on meaningful moves, fleet telemetry from a few thousand vehicles where most of the GPS jitter is uninteresting, IoT estates with flaky sensors that need smoothing before anyone trusts the readings, or dashboards where late-joining clients should not have to wait for the next event to see current state. Same primitives, different domains.

## When it is not the right tool

monoblok is a core NATS server with rules, not a replacement for a cluster. It has no JetStream storage, no clustering and no message headers on its own listener. It can read from JetStream consumers and forward subjects to a real NATS cluster, which is how the two are meant to be paired.

If plain publish and subscribe is all you need, nats-server is the better choice.

## More reading

- [The introductory blog post](https://alexjreid.dev/posts/monoblok/) covers where the idea came from, and there are [more posts about monoblok](https://alexjreid.dev/tags/monoblok/) on the blog.
- [tinyblok](https://github.com/lexvicacom/tinyblok) runs a subset of patchbay on [ESP32 boards](../esp32.html), relaying cleaned sensor data into NATS.
- The Markdown docs these pages are built from live in the [repository](https://github.com/lexvicacom/monoblok/tree/main/docs).
