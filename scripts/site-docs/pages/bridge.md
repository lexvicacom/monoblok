description: Forward conditioned subjects to a real NATS cluster, tap remote subjects, and replay JetStream streams through patchbay.

# Export, import and JetStream

{lede} monoblok is meant to be paired with a real NATS cluster. `export` forwards chosen subjects out, `import` feeds remote subjects in as rule input, and JetStream import replays history before going live.

Sensors publish to a local monoblok, patchbay rules do the conditioning, and only the tidy subjects of interest are forwarded to a remote NATS cluster. The subjects remain in the local monoblok for local subscribers. An edge monoblok can therefore do its conditioning work locally and send only the meaningful subjects on to a production NATS deployment.

Both directions connect through vendored [nats.c](https://github.com/nats-io/nats.c), so TLS and `.creds` files are supported and reconnects are handled for you. Leaving the forms out makes the runtime cost zero. Runnable versions of everything here are in [NATS integration examples](examples-nats.html).

## Export

Zero or one `(export ...)` form in the patchbay file configures the outbound forwarder. The old `(bridge ...)` spelling is deprecated but still accepted.

```edn
(export
  :servers  ["tls://connect.ngs.global:4222"]
  :creds    "/etc/monoblok/ngs.creds"
  :tls      true
  :name     "monoblok-prod-1"
  :origin-header true
  :replay-header true
  :export   ["telemetry.>" "alerts.>"])
```

{{between docs/overview.md "A local publish (from a NATS client or a patchbay rule)" "Reconnects are handled inside nats.c."}}

It is export-only: remote replies are not routed back. A good export list names the conditioned subjects, not the raw ones. Point it at `T.*.p.stable` and a JetStream stream on the far side stores deduplicated, rounded prices instead of the raw firehose: less write amplification, less retained noise, and smaller, faster replays.

{{section docs/patchbay-cheatsheet.md "Export keywords" level=3}}

## Import

Zero or one `(import ...)` form configures inbound tap mode. The flat shape is a compatibility alias for one core NATS import:

```edn
(import
  :servers ["nats://raw.example:4222"]
  :name    "monoblok-import-prod-1"
  :subject ["raw.>" "replay.>"])

(on "raw.temp"
  (publish! "clean.temp" payload))
```

Imported messages are patchbay inputs only. Direct monoblok subscribers do not see the raw imported subject unless a rule republishes it: a subscriber to `raw.>` will not see the imported `raw.temp`, only what rules emit, such as `clean.temp` above. In import mode the local socket stays open for `SUB`, `UNSUB`, `PING` and LVC and stats reads, but client `PUB` commands are rejected.

Newer configs can make the import kind explicit. `:core` entries are live core NATS subscriptions; `:streams` entries are JetStream durable consumers:

```edn
(import
  :core
  [[:servers ["nats://raw.example:4222"]
    :subject ["raw.>"]]]

  :streams
  [[:servers ["nats://js.example:4222"]
    :subject "sensors.temp"
    :stream "SENSORS"
    :consumer "monoblok-prod-1"
    :catch-up true]])
```

When export and import touch the same cluster, set `:origin-header true` on both: exported messages carry `x-monoblok: <hostname>`, and import ignores messages carrying that header, so monoblok does not consume its own output.

{{section docs/patchbay-cheatsheet.md "Import keywords" level=3}}

## JetStream

With `:streams`, monoblok consumes JetStream; it does not serve JetStream to local clients. On startup it can replay historical messages before opening its listener, using JetStream message metadata as the event-time clock, so windows, bars, counters and the last-value cache are warm before the first client connects.

{{between docs/patchbay.md "JetStream import is consumer-only: monoblok does not serve" "messages do not carry these replay headers."}}

### Telling replay from live

During JetStream catch-up, rules see `replaying?` as true. After the listener opens, `replaying?` is false. Rules that do not mention `replaying?` produce the same subjects during replay and live operation; rules that want to suppress or redirect historical output gate that behaviour explicitly:

```edn
(on "js.sensors.temp"
  (if replaying?
    (publish! "js.replay.last-temp" payload)
    (publish! "js.live.temp" payload)))
```

### Acknowledgement

Event time comes from JetStream message metadata. A message is acknowledged only after the patchbay handler and the downstream publish path succeed. Malformed subjects, oversized payloads, missing metadata, failed rule evaluation, or a failed downstream publish leave the message unacked; JetStream owns redelivery.

The JetStream worker threads never touch rule state. Each message is handed to the single event loop for evaluation, one at a time, and that handoff is backpressured.

See the [JetStream replay example](examples-nats.html#jetstream-replay) for a complete runnable file.

## Secrets and addresses from the environment

Top-level config string values may be written as `(env "NAME")`. This is load-time only, not a rule-body form, and it reads exactly one environment variable as one string. Missing or empty variables fail validation; values are not split on commas.

```edn
(export
  :servers (env "NATS_SERVERS")
  :token   (env "NATS_TOKEN")
  :export  ["sensors.>"])
```

In YAML config, write the same value as `env: "NAME"`:

```yaml
export:
  servers:
    env: "NATS_SERVERS"
  token:
    env: "NATS_TOKEN"
  export: ["sensors.>"]
```
