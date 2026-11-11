description: Patchbays that pair monoblok with a real NATS server: export, import, and JetStream replay.

# NATS integration

{lede} These examples run monoblok next to a real `nats-server`: forwarding conditioned subjects out, tapping raw subjects in, and replaying a JetStream stream through the rules before going live.

The configuration forms are described in [Export, import and JetStream](bridge.html). Each example script starts the `nats-server` it needs, so you need `nats-server` and the NATS CLI installed.

## Export to a NATS server

{tag:Intermediate} The front door pattern in its smallest form: publishers send to monoblok, and one top-level `export` form names the subjects that are forwarded to the remote server.

{{include examples/bridge.edn}}

Local subscribers are served first and the remote second, so a slow uplink cannot starve local delivery. The rule's `.echoed` publishes also match `telemetry.>`, so they are forwarded too: export applies to client publishes and rule publishes alike.

```sh
examples/bridge.sh
```

## Tap an existing NATS server

{tag:Intermediate} The reverse direction. monoblok subscribes to `raw.>` on a real NATS server, runs those messages through the rules, and exposes only the conditioned output. Here the clean stream is also exported back to the same server.

{{include examples/import.edn}}

Imported messages are private patchbay input. A direct monoblok subscriber to `raw.>` does not see the imported `raw.temp`; subscribers see only what rules publish, such as `clean.temp`. While `import` is configured, local clients can still subscribe but their `PUB` commands are rejected.

```sh
examples/import.sh
```

The YAML spelling of the same file:

{{include examples/import.yml}}

## JetStream replay

{tag:Advanced} monoblok can consume a JetStream durable consumer. On startup it replays the stream's history through the rules before opening its listener, using each message's JetStream timestamp as the clock, then carries on live with the same consumer. Windows, bars and counters are therefore warm before the first client connects.

{{include examples/jetstream.yml}}

What to notice:

- **`env: "JS_URL"`** reads the server URL from the environment when the patchbay loads, so the file holds no addresses.
- **`$replaying`** is true during catch-up and false afterwards. The first rule uses it to send historical values to `js.replay.last-temp` and live ones to `js.live.temp`; the second rule's `when: [not, $replaying]` guard keeps its deadband output live-only.
- **`[bar!, ms, 1000, ...]`** closes one-second bars on event time during replay, because the replay clock is the JetStream message timestamp.
- **`replay-header: true`** marks exported messages derived from replay with `x-monoblok-replay: true`, so the remote side can tell history from live output.
- **Acks follow success.** A message is acknowledged only after its rules and downstream publishes succeed; anything else is left for JetStream to redeliver.

```sh
examples/jetstream.sh
JS_PORT=15999 COUNT=1000 examples/jetstream.sh
```

The script starts a JetStream server and a plain NATS bridge target, populates 1000 events with a pause halfway (so the replay has enough event-time gap to close a bar), then runs monoblok through catch-up plus one live event.
