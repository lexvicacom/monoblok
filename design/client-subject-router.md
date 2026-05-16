# monoblok Client Subject Router Design

## Status

Draft / exploratory design notes.

This document describes a client-side facade for routing NATS operations to
different `monoblok` groups based on subject ownership.

The facade owns the underlying NATS client connections. Applications instantiate
the facade instead of a raw NATS connection and continue to publish, subscribe,
and request against ordinary subject names.

This is a partitioning design, not the HA mechanism. HA remains per `monoblok`
group.

---

## Motivation

`monoblok` is intentionally single-threaded. That keeps protocol parsing,
routing, patchbay state, LVC, snapshots, and connection lifetime easy to reason
about, but it also means one busy domain can consume the loop capacity of the
node serving it.

Subject ownership partitioning lets operators keep the single-threaded model
while reducing blast radius:

```text
sales.>  -> monoblok-sales group
orders.> -> monoblok-orders group
```

Each group can then have its own warm-standby HA pair:

```text
sales.>  -> sales-a ACTIVE, sales-b STANDBY
orders.> -> orders-a ACTIVE, orders-b STANDBY
```

The client facade answers only one question:

```text
which monoblok group owns this subject?
```

Normal NATS client reconnect behavior still handles failover between endpoints
inside a group.

---

## Non-Goals

This design intentionally avoids:

- changing the core NATS protocol
- patching or forking `nats.go`, `nats.py`, `nats.c`, or other upstream clients
- adding a network proxy process
- making `monoblok` active/active
- providing cross-domain transactions
- hiding cross-domain patchbay behavior behind implicit routing magic
- global queue-group balancing across multiple `monoblok` groups
- making broad wildcard subscriptions free

The facade should use upstream NATS clients as ordinary dependencies and keep
the subject-routing policy in a small Monoblok-owned layer.

---

## High-Level Model

```text
application
    |
    v
monoblok subject-router facade
    |
    +-- route sales.>  -> nats client -> nats://sales-a:4222,nats://sales-b:4222
    |
    +-- route orders.> -> nats client -> nats://orders-a:4222,nats://orders-b:4222
    |
    +-- route inv.>    -> nats client -> nats://inv-a:4222,nats://inv-b:4222
```

The facade owns:

- route table parsing and validation
- dialing all configured route groups
- choosing one group for concrete-subject operations
- fanning out wildcard subscriptions when needed
- closing or draining all underlying clients

The underlying NATS clients own:

- socket I/O
- reconnect behavior inside a route group
- TLS, auth, credentials, and server lists
- ping/pong and connection liveness
- per-connection subscription state
- request inbox mechanics, where available

---

## Why Not A Network Proxy

A network proxy could route subjects without changing application code. Clients
would connect to one NATS-shaped endpoint, and the proxy would forward traffic
to the owning `monoblok` group.

That transparency has real cost:

- the proxy becomes another runtime component to deploy and observe
- all client traffic now crosses an extra hop
- proxy capacity can become the new bottleneck
- proxy outages can become a single point of failure
- making the proxy highly available reintroduces its own failover story
- request/reply, queue groups, drains, and backpressure must be implemented or
  preserved correctly in the proxy

The client facade keeps the partition map at the edge and lets each application
connect directly to the `monoblok` groups it uses. There is no central routing
process in the data path, and each route group still uses ordinary NATS client
reconnect behavior across its active/standby endpoints.

The tradeoff is that applications must opt into the facade. That is acceptable
for the first design because it keeps the operational model smaller and avoids
turning the router into another service that needs its own HA design.

---

## Route Configuration

Routes map a NATS subject pattern to one endpoint group:

```text
sales.>  = nats://sales-a:4222,nats://sales-b:4222
orders.> = nats://orders-a:4222,nats://orders-b:4222
inv.>    = nats://inv-a:4222,nats://inv-b:4222
```

A route pattern uses normal NATS wildcard syntax:

- `*` matches one token
- `>` matches the rest of the subject

For operational clarity, route patterns should normally be simple ownership
prefixes ending in `>`.

Concrete subject routing uses most-specific match. If two route patterns match a
concrete subject with equal specificity, facade initialization fails.

Example:

```text
sales.vip.> -> monoblok-sales-vip
sales.>     -> monoblok-sales-general
```

`sales.vip.created` routes to `monoblok-sales-vip`.
`sales.lead.created` routes to `monoblok-sales-general`.

Unknown concrete subjects fail closed. The facade must not publish them to a
default group unless the configuration explicitly contains a catch-all route.

---

## Connection Ownership

The first implementation should keep ownership simple:

- the facade creates all underlying NATS clients
- the facade closes or drains all clients it created
- applications do not pass existing raw NATS connections into the facade

This keeps lifecycle, reconnect callbacks, and metrics behavior easier to make
consistent across route groups.

Future clients may add a borrowed-connection mode if real applications need to
reuse custom NATS connection setup, but that should not be part of the first
shape.

---

## Client API Shape

The facade should expose a small NATS-like surface rather than trying to be a
perfect drop-in replacement for every upstream client type.

Minimum useful operations:

```text
Connect(routes, options)
Publish(subject, payload)
PublishRequest(subject, reply, payload)
Subscribe(subject, callback)
QueueSubscribe(subject, queue, callback)
Request(subject, payload, timeout)
Flush()
Drain()
Close()
```

The exact names should follow the host language where practical.

The important rule is that callers address real subjects directly:

```text
publish "sales.lead.created"
subscribe "orders.created"
request "sales.lookup"
```

The facade performs route selection internally.

---

## Publish Semantics

`Publish(subject, payload)` requires a concrete publish subject.

Flow:

```text
validate subject
find most-specific owning route
publish through that route's NATS client
```

If no route owns the subject, return an error.

If the selected route's NATS client is disconnected, use the underlying client
library's normal behavior. The facade should not invent a second buffering or
retry system above the NATS client.

---

## Request / Reply Semantics

`Request(subject, payload, timeout)` routes by the request subject and delegates
the request to that route's underlying NATS client.

This keeps the generated inbox subscription and reply handling on the same
connection:

```text
request sales.lookup
    -> sales route connection
    -> underlying NATS client creates inbox on sales connection
    -> response returns on sales connection
```

Manual `PublishRequest(subject, reply, payload)` is more dangerous because the
reply subject may imply a different ownership domain. The v1 facade can support
it only as a low-level escape hatch and should document that the caller is
responsible for making the reply subject reachable on the selected route.

Applications should prefer `Request`.

---

## Subscribe Semantics

Subscriptions are routed by subject interest, not by a concrete publish subject.

If the subscription pattern is wholly owned by one route, create one underlying
subscription.

```text
subscribe sales.created
    -> sales route only
```

If the subscription pattern can match subjects from multiple route groups, fan
out to all matching routes and merge callbacks in the facade.

```text
subscribe >
    -> sales route
    -> orders route
    -> inv route
```

This is useful but not free. A broad subscription consumes subscription state
and callback work on every matching group.

The facade should return one logical subscription handle that can unsubscribe
all underlying subscriptions.

---

## Queue Subscriptions

Queue subscriptions work cleanly when the subject interest maps to one route
group:

```text
QueueSubscribe("sales.created", "workers", cb)
```

The queue group is then scoped to the selected `monoblok` group and behaves like
a normal NATS queue subscription.

Broad queue subscriptions that fan out across groups are allowed only with
explicit documentation:

```text
QueueSubscribe(">", "workers", cb)
```

This creates one queue subscription per route group. It does not create a single
global queue group. A busy route group can still deliver more messages than
another group, and balancing occurs independently inside each group.

If that behavior is too surprising, the first implementation should reject
fanout queue subscriptions and require callers to subscribe per domain.

---

## Flush, Drain, and Close

`Flush()` should flush every underlying route client and return the first error,
while preserving enough detail for diagnostics.

`Drain()` should drain every underlying route client. The facade should avoid
starting new subscriptions or publishes after drain begins.

`Close()` should close every underlying route client created by the facade.

Because the facade owns all connections in v1, lifecycle behavior is explicit
and local.

---

## Errors

The facade should fail early on configuration errors:

- invalid subject pattern
- empty route list
- duplicate route name, if names are used
- ambiguous equal-specificity ownership
- invalid endpoint URL
- route with no endpoints

Runtime errors should make the route visible:

```text
no route owns subject "billing.invoice.created"
route "orders" publish failed: ...
route "sales" request timed out: ...
```

Do not silently fall back to another route group. Subject ownership is the
partition boundary.

---

## Cross-Domain Behavior

The facade routes client operations. It does not make a `monoblok` process span
multiple domains.

If a patchbay rule inside the `sales` group needs to emit into `orders`, that
should be modeled as an explicit bridge/export path, not as implicit client
facade behavior.

This keeps ownership and failure boundaries visible:

```text
sales input
    -> sales monoblok
    -> explicit bridge/export
    -> orders monoblok
```

Cross-domain flows should be observable and configured deliberately.

---

## Relationship To Warm Standby HA

The warm-standby HA design remains local to a route group.

```text
client facade route:
  sales.> -> nats://sales-a:4222,nats://sales-b:4222

inside that endpoint list:
  sales-a ACTIVE
  sales-b STANDBY
```

The facade does not decide active vs standby. It gives the underlying NATS
client the endpoint list for the group and relies on the HA design's inactive
node behavior:

```text
-ERR 'monoblok not active'
```

followed by reconnect elsewhere.

This means route partitioning and HA stay orthogonal:

- route table decides subject ownership
- each route group decides active/standby service

---

## Go Client Shape

Go is a good first production reference because `nats.go` is widely used and
goroutines make multiple underlying connections straightforward.

Sketch:

```go
routes := []monoblok.Route{
    {
        Pattern: "sales.>",
        URLs:    []string{"nats://sales-a:4222", "nats://sales-b:4222"},
    },
    {
        Pattern: "orders.>",
        URLs:    []string{"nats://orders-a:4222", "nats://orders-b:4222"},
    },
}

mb, err := monoblok.Connect(routes, monoblok.WithName("orders-api"))
if err != nil {
    return err
}
defer mb.Close()

err = mb.Publish("sales.lead.created", payload)
sub, err := mb.Subscribe("orders.created", handler)
reply, err := mb.Request("sales.lookup", payload, time.Second)
```

The Go facade should not try to masquerade as `*nats.Conn`, because `*nats.Conn`
is a concrete upstream type. It can expose a compatible-enough API for Monoblok
applications.

---

## Python Client Shape

Python is useful for a fast prototype and for smoke tests.

Sketch:

```python
routes = [
    Route("sales.>", ["nats://sales-a:4222", "nats://sales-b:4222"]),
    Route("orders.>", ["nats://orders-a:4222", "nats://orders-b:4222"]),
]

mb = await monoblok.connect(routes, name="orders-api")

await mb.publish("sales.lead.created", payload)
sub = await mb.subscribe("orders.created", cb=handler)
msg = await mb.request("sales.lookup", payload, timeout=1.0)
```

Duck typing can make this feel close to a normal Python NATS client, but the
facade should still document which cross-route semantics differ from a single
connection.

---

## C Client Shape

C can come later if the model proves useful.

The C facade should be a small API over `nats.c`, not a fork of `nats.c`:

```c
mb_client *mb = NULL;
mb_client_connect(&mb, routes, route_count, &opts);
mb_client_publish(mb, "sales.lead.created", data, data_len);
mb_client_close(mb);
```

Memory ownership, callback lifetime, and subscription fanout make C more
expensive to get right, so it should not be the first reference
implementation.

---

## Operational Concerns

All applications using the facade must share a consistent route table for
correctness.

Useful operational features:

- route table version or digest in logs
- per-route connection status
- per-route publish, subscription, request, and error counters
- clear startup error when route config is invalid
- optional dry-run command that prints the owning route for sample subjects
- optional route table reload only if it can be made atomic and boring

Dynamic route changes are risky. The first implementation can require restart
on route table changes.

---

## MVP

A useful first implementation only needs:

- route config with prefix-style NATS patterns
- facade-owned connections
- concrete `Publish`
- single-route `Subscribe`
- logical unsubscribe handle
- `Request` using the selected route's native request API
- `Flush`
- `Close`
- fail-closed unknown-subject errors

Then add:

- fanout wildcard subscriptions
- queue subscriptions
- drain
- route metrics
- route table digest / version

---

## Open Questions

- Should fanout queue subscriptions be rejected in v1?
- Should broad `Subscribe(">")` require an explicit option?
- Should route config allow a catch-all `>` route, or discourage it?
- How should route specificity be scored for mixed wildcard patterns?
- Should route groups have stable names in config, or are patterns enough?
- Should route table reload be supported, or should applications restart?
- Should the facade expose per-route raw clients for diagnostics, or keep them
  fully hidden?
