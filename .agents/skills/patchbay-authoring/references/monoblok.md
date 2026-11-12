# Monoblok daemon features

## Last values and the nil-safe idiom

`(last SUBJECT)` returns the latest payload published on a literal SUBJECT as a
string, or nil when there is none. It reads the LVC, so SUBJECT must match a
top-level `(lvc ...)` filter (without one it is always nil). Arithmetic and
comparison on nil are type errors, so use a thread, which stops on nil:

```edn
(lvc ["temp.>"])

(on "temp.inside"
  (-> (last "temp.outside")
      (- payload-float)
      (publish! "temp.difference" :dp 1)))
```

`last` returns a string: arithmetic coerces numeric strings, but `:dp N` formats only numbers, so `(reply! :dp 1 (last "x"))` fails at run time. Wrap it, e.g. `(round 3 (last "x"))`, or omit `:dp`.

Thread-last puts the `last` value in the final argument, so this publishes
`payload - outside`. Do not write `(- (last "temp.outside") payload-float)`
unguarded; if you need the non-threaded form, guard it with `(and (last S) ...)`.

## Request-reply (monoblok)

`reply-to` is the request's reply subject (nil when the message had none).
`(reply! VALUE)` and `(reply! :dp N VALUE)` publish VALUE to it. It is a silent
no-op when there is no reply subject or VALUE is nil, and it threads like
`publish!`. `reply-to` is nil for imported messages, clock ticks, `every`
bodies, and re-entered evaluations, and always nil under `--soundcheck`.

```edn
(lvc ["temp.>"])

; nats req q.temp "" answers with the latest kitchen temperature.
(on "q.temp"
  (reply! (last "temp.kitchen")))
```

## Heartbeats: `every`

`(every :ms N [:reentrant BOOL] BODY...)` is top level, a sibling of `on`, and
never appears inside a rule body. `N` is a literal integer from 1 to 1e12. The
first tick schedules now + N without firing, and a missed interval fires once,
never a burst. Inside the body `subject` and `payload` are empty strings and
`reply-to` is nil. Use `(count! :subject "x.count")` rather than bare
`(count!)` there, since the default subject would be empty.

```edn
(every :ms 5000
  (publish! "monoblok.heartbeat" "alive"))
```

## Export form (optional, zero or one at top level)

```edn
(export
  :servers ["tls://a.example:4222" "tls://b.example:4222"]
  :creds   "/etc/monoblok/ngs.creds"
  :tls     true
  :name    "monoblok-prod-1"
  :origin-header true
  :replay-header true
  :export  ["telemetry.>" "alerts.>"])
```

Deprecated `(bridge ...)` is still accepted as a compatibility alias.
Other keywords: `:user` / `:password`, `:token`, `:tls-ca`, `:tls-cert`
/ `:tls-key`, `:tls-skip-verify` (dev only), `:connect-timeout-ms`,
`:ping-interval-ms`, `:max-reconnect` (-1 unlimited), `:reconnect-wait-ms`.
`:origin-header true` adds `x-monoblok: <hostname>` to forwarded messages.
`:replay-header true` adds `x-monoblok-replay: true` and
`x-monoblok-assumed-ts: <unix-ms>` to bridged replay output.
`:export` is a vector of subject filters; matched publishes are
forwarded as-is. Nothing flows back. Both `:servers` and `:export`
also accept the legacy `(...)` list syntax, but `[...]` is the
recommended form. LVC follows the same recommendation:
`(lvc ["sensors.>" "alerts.>"])`, with legacy `(lvc "sensors.>")`
still accepted.

String-valued top-level config positions can use `(env "NAME")`, for example
`:servers (env "NATS_SERVERS")` or `:token (env "NATS_TOKEN")`. Env values are
read once at patchbay load time, must be non-empty, and are not comma-split.

## Import form (optional, zero or one at top level)

```edn
(import
  :servers ["tls://a.example:4222" "tls://b.example:4222"]
  :creds   "/etc/monoblok/ngs.creds"
  :name    "monoblok-import-prod-1"
  :origin-header true
  :subject ["raw.>" "replay.>"]
  :max-pending 4096)
```

Connection keywords match `(export ...)`. `:subject` is a string or vector of
remote subject filters to subscribe to. With `(import ...)` configured, local
socket clients may still subscribe but client `PUB` commands are rejected.
Imported messages run through `(on ...)` rules but are not routed to local
subscribers as raw publishes; only explicit rule outputs become visible.
`:origin-header true` ignores remote messages that carry monoblok's
`x-monoblok` header. `:max-pending` bounds the NATS-to-loop queue; default is
4096 messages. `:subjects` is accepted as an alias for `:subject`. Import
counters are published as `$STATS.import.received`, `.processed`, `.dropped`,
and `.failed`.

The flat shape above is one core NATS import. The nested shape scopes several
sources, each entry a vector with its own connection keywords:

```edn
(import
  :core    [[:servers ["nats://a.example:4222"] :subject ["raw.>"]]]
  :streams [[:servers ["nats://a.example:4222"]
             :subject  "sensors.>"
             :stream   "SENSORS"
             :consumer "monoblok-sensors"
             :catch-up true]])
```

`:streams` entries consume JetStream durable pull consumers. Each entry takes
one subject filter plus `:stream` and `:consumer` (both required) and optional
`:catch-up` (default true). monoblok is a JetStream consumer only, never a
JetStream server. With catch-up on, the stream is replayed up to its startup
high-water before the local listener opens, using each message's JetStream
timestamp as the clock, so `:ms` windows and time bars close on event time.
During that phase rules see `replaying?` as true; afterwards the same consumer
continues live with `replaying?` false. A rule that should not act on history
(alerts, notifications) guards itself with `(when (not replaying?) ...)`. A
message is acked only after its rules and downstream publishes succeed, so a
failing rule causes redelivery.
