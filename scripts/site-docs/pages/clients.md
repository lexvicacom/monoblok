description: Which parts of NATS monoblok speaks, plus optional auth, TLS and the HTTP/SSE adapter.

# NATS clients, auth and HTTP

{lede} monoblok speaks enough of the NATS wire protocol that any NATS client can connect. You can drop it in alongside existing tooling without writing a client library first.

## NATS support

monoblok implements the NATS core pieces it needs to behave like a small broker.

| Feature | Support |
|---|---|
| `PUB` / `SUB` / `UNSUB` / `MSG` | yes |
| wildcards | yes |
| request/reply / reply-to | yes, local core reply-to; bridge remains export-only |
| queue groups | yes |
| headers | no |
| `$LVC.*` last-value replay | yes, monoblok extension |
| `$STATS.*` live counters | yes, monoblok extension |
| bridge to real NATS | export-only |
| import from real NATS | yes, as private patchbay ingress |
| JetStream import | yes, as consumer-only patchbay ingress |
| TLS on the local server | yes, optional server cert/key |
| auth on the local server | yes, optional global token or user/pass from environment |
| HTTP/SSE adapter | yes, optional plain HTTP listener |
| JetStream service for local clients | no |
| clustering | no |

Request-reply works through patchbay as well as plain pub/sub. A rule can read the request's reply subject as `reply-to`, and `(reply! VALUE)` publishes to it, so `(on "q.temp" (reply! (last "temp.kitchen")))` makes `nats req q.temp ""` answer with the latest cached kitchen temperature. `reply-to` is nil for imported messages, clock ticks, and `every` bodies. See [Request-reply](forms.html#request-reply).

Without auth flags, `CONNECT` bodies are accepted and ignored. `+OK` is never sent. Client publishes to `$LVC.*` and `$STATS.*` are rejected, and while an [`import`](bridge.html#import) form is configured all client `PUB` commands are rejected.

{{section docs/overview.md "Simple auth for local NATS clients" level=2}}

{{section docs/overview.md "TLS for local NATS clients" level=2}}

{{section docs/overview.md "HTTP/SSE adapter" level=2}}
