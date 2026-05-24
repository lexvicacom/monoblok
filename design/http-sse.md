# monoblok HTTP/SSE Design

## Status

Draft / exploratory design notes.

This document describes a small HTTP surface for browser-friendly reads and
simple writes without adding WebSockets or a large HTTP framework.

The goal is not to make `monoblok` a general HTTP server. The goal is a narrow
adapter:

- `GET` opens one Server-Sent Events subscription
- `POST` publishes one payload to one subject
- auth reuses the local token or user/pass configuration
- HTTPS is handled by a fronting proxy, not by monoblok v1

---

## Motivation

WebSockets are useful when both sides need long-lived bidirectional messaging,
but the first browser-facing use case for monoblok is simpler:

- dashboards and small tools subscribe to conditioned subjects
- forms, curl, or fetch publish one value to one subject
- payloads are expected to be text-like values such as numbers, labels, or JSON

SSE fits that shape. It is a plain HTTP response, works naturally with browser
`EventSource`, survives through ordinary reverse proxies, and keeps the daemon
away from WebSocket framing, masking, fragmentation, and protocol negotiation.

---

## Non-Goals

This design intentionally avoids:

- WebSocket or WSS support
- in-process HTTPS for the HTTP listener in v1
- arbitrary binary payload support over SSE
- chunked request bodies
- multipart/form-data
- query-string control surfaces
- serving static files
- request/reply over HTTP
- multiple subscriptions on one SSE request
- per-subject authorization or user maps
- adding a large HTTP dependency

The HTTP surface should remain an adapter over the existing router and publish
path, not a second application model.

---

## Listener

The HTTP listener should be separate from the NATS listener:

```sh
monoblok --http-host 127.0.0.1 --http-port 8080
```

Default should be disabled unless `--http-port` is set. A separate listener is
simpler than multiplexing because raw NATS is server-first (`INFO`), while HTTP
is client-first (request line and headers).

Plain HTTP is enough for v1. Production HTTPS should be terminated by Caddy,
nginx, a load balancer, or another fronting proxy:

```text
browser/client -> HTTPS proxy -> http://127.0.0.1:8080
```

If standalone HTTPS is needed later, add separate `--http-tls-cert` and
`--http-tls-key` flags. Do not reuse the NATS `--tls-cert` / `--tls-key` flow:
NATS TLS starts after plaintext `INFO`, while HTTPS starts before the HTTP
request.

---

## Path Model

Favor paths over query strings. Path tokens map to NATS subject tokens.

```text
GET  /sub/sensors/temp       -> SUB sensors.temp
GET  /sub/sensors/%3E        -> SUB sensors.>
GET  /sub/$LVC/sensors/temp  -> SUB $LVC.sensors.temp

POST /pub/sensors/temp       -> PUB sensors.temp <body>
```

Path decoding rules:

- split the route prefix from the subject path first
- percent-decode each subject path segment
- reject empty subject tokens
- join decoded subject tokens with `.`
- validate with normal NATS subject rules
- allow wildcards only on `/sub/...`

Literal `/` inside a subject token should be percent-encoded by the client if
it is ever needed. In normal monoblok usage, `/` is just the HTTP hierarchy and
`.` is the NATS hierarchy.

---

## SSE Subscribe

An SSE subscription is a normal router subscription with a different output
writer.

```http
GET /sub/sensors/%3E HTTP/1.1
Authorization: Bearer sekret
```

Response:

```http
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
```

Each publish is emitted as one SSE event:

```text
event: msg
data: {"subject":"sensors.temp","payload":"31.2"}

```

The JSON data envelope carries the actual published subject, which matters for
wildcard subscriptions and `$LVC.>` replay. The payload is a JSON-escaped text
string:

```text
event: msg
data: {"subject":"logs.app","payload":"first line\nsecond line"}

```

Payload constraints:

- reject or drop SSE delivery for payloads containing NUL
- JSON-escape control characters, quotes, and backslashes
- do not base64-encode in v1
- optionally validate UTF-8 later if real clients need stricter behavior

The implementation should send occasional comment heartbeats if needed to keep
proxies from closing quiet streams:

```text
: keepalive

```

Heartbeat cadence should be conservative and configurable only if operators
actually need it.

---

## POST Publish

`POST /pub/...` is an internal client publish. It should use the same behavior
as a local NATS client `PUB`, not the import path.

```http
POST /pub/sensors/temp HTTP/1.1
Content-Length: 4
Authorization: Bearer sekret

31.2
```

Equivalent NATS operation:

```text
PUB sensors.temp 4\r\n31.2\r\n
```

The publish must go through the existing client publish checks:

- validate subject
- reject if import mode disables client publishes
- reject `$LVC.*` writes
- reject `$STATS.*` writes
- call the router publish path
- increment `total_pubs`
- run patchbay evaluation
- reschedule patchbay timers

Successful POST publishes fan out to ordinary NATS subscribers, SSE
subscribers, LVC, bridge, and patchbay exactly like a NATS `PUB`.

Request body rules:

- require `Content-Length`
- cap at `MB_MAX_PAYLOAD`
- require `Content-Type: text/plain` or `Content-Type: application/json`
- reject NUL if the HTTP surface remains text-only
- do not support chunked bodies in v1
- do not infer subject or options from query parameters

Suggested statuses:

```text
202 Accepted                  publish accepted
400 Bad Request               invalid path, subject, headers, or body
401 Unauthorized              missing or wrong HTTP auth
403 Forbidden                 client publishes disabled in import mode
409 Conflict                  $LVC or $STATS read-only subject
413 Payload Too Large         body exceeds cap
415 Unsupported Media Type    POST content type is not text/plain or application/json
500 Internal Server Error     router or patchbay failure
```

---

## Auth

Reuse the local auth configuration:

```text
--auth-token-env ENV
--auth-user-env ENV --auth-pass-env ENV
```

HTTP auth mapping:

- token mode accepts `Authorization: Bearer <token>`
- user/pass mode accepts `Authorization: Basic <base64(user:pass)>`
- no auth config means HTTP auth is not required

Missing or wrong credentials should return `401 Unauthorized`. Include a
`WWW-Authenticate` header for Basic mode:

```text
WWW-Authenticate: Basic realm="monoblok"
```

Do not add per-subject permissions in this feature. If operators need HTTP
exposure beyond trusted local networks, put monoblok behind a proxy that owns
TLS, IP allowlists, rate limits, and access logs.

---

## Router Integration

Treat SSE streams as normal subscriptions. The router should not grow a
parallel SSE fanout path.

The one needed abstraction is that router delivery should stop assuming every
connection wants NATS `MSG` frames. Add a small writer hook to
`mb_router_conn`, with the default preserving current behavior:

```c
bool (*write_msg_fn)(void *ctx, mb_buf *out,
                     const char *prefix, size_t prefix_len,
                     mb_slice subject,
                     mb_slice sid,
                     mb_slice reply_to,
                     mb_slice payload);
void *write_msg_ctx;
```

NATS connections leave the hook unset and get the current `MSG` serialization.
SSE connections set the hook to write one `event: msg` frame. The router still
owns matching, LVC replay, queue selection if used later, slow-consumer
accounting, and kick scheduling.

On HTTP/SSE connection close, call `mb_router_remove_all_for()` with the SSE
connection's router-facing object, exactly like the NATS connection path.

POST publish handling should be factored so NATS `PUB` and HTTP `POST` share
one local helper for client-publish semantics. That avoids drift around import
mode, read-only subjects, stats, patchbay eval, and timer rescheduling.

---

## Parser And Bounds

Implement only enough HTTP/1.1 for this adapter:

- request line
- headers
- `Content-Length`
- `GET`
- `POST`
- no chunked request bodies
- no pipelining requirement
- bounded header bytes
- bounded path length
- bounded request body

The parser can be small and local. It should be explicit about partial reads and
limits, similar to the NATS protocol parser. Avoid bringing in a framework for
this v1 surface.

Keep HTTP connection state separate from NATS connection state. Sharing the
router-facing pieces is desirable; sharing protocol parser state is not.

---

## Open Questions

- Should SSE delivery reject invalid UTF-8, pass bytes through as-is, or only
  reject NUL?
- Should `/sub/...` replay `$LVC` only through explicit `$LVC` paths, or should
  there be a convenience path later?
- Should POST accept `Content-Type: application/json` only as documentation, or
  ignore content type entirely?
- Should HTTP expose a small `/health` endpoint, or should that stay out of the
  adapter?
- Should SSE heartbeat cadence be fixed, configurable, or omitted until a proxy
  requires it?
