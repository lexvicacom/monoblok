# monoblok Warm Standby HA Design

## Status

Draft / exploratory design notes.

This document describes a simple warm-standby model for `monoblok`.

The v1 design is deliberately manual:

- publishers are configured with both monoblok endpoints
- only one monoblok is active at a time
- the active node directly mirrors accepted input frames to the standby
- operators explicitly stand down one node and promote the other
- there is no distributed consensus inside `monoblok`

The goal is fast, understandable failover while preserving the single-writer
conditioning model.

---

## Non-Goals

This design intentionally avoids:

- active/active operation
- automatic failover in v1
- split-brain reconciliation
- distributed consensus
- a third monoblok acting as a magic coordinator
- synchronous standby replication on the hot publisher path
- relying on downstream emitted output as the standby state source

Automatic promotion can be added later around the same replication mechanism,
but it should not be required for the first useful version.

---

## Design Goals

- Keep exactly one active writer per conditioning domain.
- Let a standby stay warm by replaying the active node's accepted input.
- Let an operator switch active service with a small, inspectable runbook.
- Rely on normal NATS client reconnect behavior across configured endpoints.
- Avoid letting a slow standby slow down the active node.
- Bound and expose standby lag.
- Keep all shared state mutation on the existing `xev.Loop` thread.
- Keep the operational model boring enough to debug under pressure.

---

## High-Level Model

```text
publishers configured with:
  nats://monoblok-a:4222,nats://monoblok-b:4222

               publisher traffic
                      |
                      v
              ACTIVE monoblok A
                data port :4222
                      |
                      +--> conditioned downstream output
                      |
                      +--> direct accepted-frame mirror
                                |
                                v
                         STANDBY monoblok B
                           data port :4222
                           output disabled
```

The standby shadow-processes the same accepted input stream as the active node.

The standby is not active/active. It does not accept publisher writes and does
not emit downstream output until promoted.

---

## Publisher Endpoint Model

Publishers are configured with both monoblok data endpoints:

```text
nats://monoblok-a:4222,nats://monoblok-b:4222
```

Correctness must not depend on the order in which a client tries endpoints.
Some NATS clients randomize their server list unless randomization is disabled.

Any endpoint may receive a publisher connection attempt. Only the active node
may accept publisher traffic.

An inactive data endpoint should reject publisher traffic explicitly:

```text
-ERR 'monoblok not active'
```

Then it should close the connection.

Do not silently drop publishes. Core NATS publishes do not provide per-message
acknowledgement, so silent drop can make a publisher believe data was accepted
when it was not.

Inactive nodes should not buffer publisher writes. If a node is not active, it
should tell the client to reconnect elsewhere.

---

## Node Modes

### ACTIVE

An `ACTIVE` node may:

- accept publisher writes on the data listener
- mutate conditioning state
- publish downstream output
- mirror accepted input frames to a standby peer
- respond to control requests

### STANDBY

A `STANDBY` node may:

- receive mirrored input from the active node
- run the normal conditioning pipeline internally
- maintain warm state
- receive snapshots or checkpoints
- respond to control requests
- promote only after an explicit operator command

A `STANDBY` node must not:

- accept publisher writes on the data listener
- publish downstream output

### STAND_DOWN

`STAND_DOWN` is used when a node is intentionally leaving active service.

A node in `STAND_DOWN` should:

- reject new publisher writes
- close publisher connections
- stop downstream publishing
- stop sending mirror frames
- remain inspectable through the control listener

### RECOVERING

`RECOVERING` is used during bootstrap or snapshot restore.

A recovering node should:

- load its latest valid snapshot or checkpoint if configured
- validate config hash and snapshot format
- connect to the active node's mirror stream if possible
- become `STANDBY` only after validation succeeds

### STALE

`STALE` is used when a standby fell too far behind.

A stale node should:

- reject publisher writes
- keep downstream output disabled
- report why it is stale
- require a fresh snapshot before promotion is allowed

---

## Core Invariant

Only one node may accept publisher writes for a conditioning domain.

A standby may:

- receive mirrored accepted input frames
- run the normal conditioning pipeline internally
- maintain warm conditioning state
- receive snapshots or checkpoints
- answer control requests

A standby must not:

- accept publisher writes
- mutate state from direct publisher connections
- publish conditioned downstream output
- promote without an explicit operator command

Manual failover keeps the authority outside the daemon: the operator, init
system, load balancer, or deployment platform must ensure only one node is
promoted.

---

## Control Listener

Each node may expose a second listener for operator control.

Example:

```text
data listener:    nats://monoblok-a:4222
control listener: nats://monoblok-a:8222
```

The control listener speaks a small NATS-shaped request/reply protocol. This
allows operators to use familiar NATS tooling while keeping admin commands off
the data listener.

Control subjects should use the short reserved `$MBLK` prefix.

Example requests:

```sh
nats --server nats://monoblok-a:8222 request '$MBLK.STATUS' '{}'
nats --server nats://monoblok-a:8222 request '$MBLK.STAND_DOWN' '{}'
nats --server nats://monoblok-b:8222 request '$MBLK.PROMOTE' '{}'
```

Possible control subjects:

```text
$MBLK.STATUS
$MBLK.STAND_DOWN
$MBLK.PROMOTE
$MBLK.SNAPSHOT.GET
$MBLK.SNAPSHOT.PUT
```

The control listener should:

- accept enough core NATS protocol to support request/reply
- be available in `ACTIVE`, `STANDBY`, `STAND_DOWN`, `RECOVERING`, and `STALE`
- return structured status and command results
- reject unknown control subjects
- allow binary payloads for snapshot transfer
- avoid patchbay routing
- keep any control-plane LVC separate from the data listener's LVC
- avoid downstream conditioned output

The control listener has two kinds of subjects:

- read-only event subjects: `$MBLK.EVENT.>`
- request/reply control subjects: `$MBLK.STATUS`, `$MBLK.PROMOTE`,
  `$MBLK.STAND_DOWN`, `$MBLK.SNAPSHOT.*`

Read-only event subjects may be exposed more broadly on a trusted LAN if that is
useful for dashboards or debugging. Request/reply control subjects are an admin
surface. They must not be exposed like the data listener; in v1 they should be
bound to localhost, a private management address, or otherwise firewalled so only
trusted operators and peer monoblok nodes can reach them.

Request/reply control subjects should also require a simple shared secret for
v1. The secret is configured on each peer and on the operator client:

```sh
monoblok --port 4222 --control 8222 --control-token-file /etc/monoblok/control.token

monoblok status nats://monoblok-a:8222 --control-token-file /etc/monoblok/control.token
```

Because the control listener speaks NATS-shaped protocol, the simplest auth
carrier is the `CONNECT` payload:

```json
{
  "user": "monoblok-control",
  "pass": "<shared secret>"
}
```

or:

```json
{
  "auth_token": "<shared secret>"
}
```

Use one form and keep it compatible with common NATS tooling where practical.
If authentication fails, the control listener should return `-ERR` and close the
connection before accepting any request/reply control subject.

This shared secret is only an access check. It does not encrypt traffic or hide
snapshot contents. Deployments that cross untrusted networks still need a
private network, tunnel, TLS terminator, or future native TLS support.

The control listener should run on the same `xev.Loop` as the data listener.
It is a second socket, not a second event loop.

```text
one xev loop
  data TCP listener
  control TCP listener
  timers
  router state
  mode state
  mirror state
```

Keeping one loop preserves the existing invariant that shared router, server,
rule, LVC, and mode state are mutated only on the loop thread.

Implementation note: the control listener should use a separate control
connection state machine rather than parameterizing the existing data-plane
connection path. It can reuse the NATS parser and serializers, but it should
have its own small subscription table / control LVC and should not route through
the main router or patchbay evaluator.

---

## Same Binary Control Commands

`monoblokctl` does not need to be a separate shipped binary. Control commands
can live in `monoblok` itself.

Possible CLI shape:

```sh
monoblok --port 4222 --control 8222 --patchbay patchbay.edn

monoblok status nats://monoblok-a:8222
monoblok stand-down nats://monoblok-a:8222
monoblok status nats://monoblok-b:8222
monoblok promote nats://monoblok-b:8222
```

The daemon mode remains the normal server path. The control subcommands are
thin clients that send request/reply commands to a node's control listener.

---

## Manual Switchover

Normal state:

```text
A: ACTIVE, accepts publishers, emits output, mirrors accepted frames to B
B: STANDBY, rejects publishers, output disabled, tracks mirror lag
```

Planned manual switch:

```sh
monoblok stand-down nats://monoblok-a:8222
monoblok status nats://monoblok-b:8222
monoblok promote nats://monoblok-b:8222
```

Detailed flow:

```text
operator asks A to stand down
A rejects new publisher writes
A closes publisher connections
A stops downstream output
A stops sending mirror frames
A reports mode=STAND_DOWN

publishers reconnect through their configured endpoint list
inactive endpoints reject

operator checks B status
B reports mode=STANDBY
B reports mirror lag / last sequence / config hash
B reports promotion_eligible=true

operator promotes B
B enables downstream output
B starts accepting publisher writes
B reports mode=ACTIVE
```

This creates a brief reconnect window. That is acceptable for v1.

---

## Emergency Failover

If A is dead or unreachable:

```sh
monoblok status nats://monoblok-b:8222
monoblok promote --force nats://monoblok-b:8222
```

`--force` should be intentionally loud. The operator must ensure A cannot still
accept publisher traffic.

Examples of acceptable fencing:

- A process is stopped
- A host is powered off
- A data listener is removed from service discovery or load balancing
- firewall rules prevent publishers from reaching A
- the deployment platform has terminated A

Forced promotion should still check:

- config hash
- standby mode
- stale status
- latest mirrored sequence
- mirror lag or snapshot age if known

The command may allow override of freshness checks, but the response must make
the risk explicit.

---

## Accepted-Frame Mirroring

The standby is kept warm by mirroring accepted input frames from the active node.

Mirroring occurs after:

- socket read
- protocol parse
- frame validation
- accepted publisher input decision

Mirroring occurs before or at the same early handoff point as the normal
conditioning path.

Conceptually:

```text
socket bytes
    -> protocol parse
    -> validated accepted PUB frame
    -> enqueue mirror frame
    -> normal conditioning pipeline
```

The mirrored unit is not arbitrary TCP bytes. It is a canonical accepted input
frame.

Example mirrored frame fields:

```text
subject
reply subject, if present
payload bytes
accepted sequence number
received timestamp, optional
config hash / epoch marker, optional
```

Each mirrored frame must carry a monotonically increasing `accepted_seq`.
This sequence is assigned by the active node after it accepts a publisher frame
and before it enters the normal conditioning path.

The standby replays these accepted frames through the normal `monoblok`
conditioning pipeline with output disabled.

---

## Why Mirror Accepted Input Instead of Output

Mirroring only emitted output is simpler, but it is too lossy for warm standby.

For example, the active node may receive:

```text
10.0, 10.1, 10.2, 10.3
```

and emit only:

```text
10.3
```

If the standby only receives the emitted output, it does not know about
suppressed values, debounce windows, smoothing state, or intermediate filter
state.

Mirroring accepted input lets the standby run the same conditioning logic and
maintain comparable internal state.

Optional future enhancement: mirror active emitted output as a validation stream
so the standby can compare its shadow output against the active node's real
output.

---

## Hot Path Requirements

The publisher hot path must not block on standby replication.

The active node should do only a cheap bounded enqueue into the mirror path.

Hot path shape:

```text
parse
-> validate
-> accept
-> enqueue mirror frame
-> continue
```

The hot path must not:

- write directly to standby sockets
- wait for standby ACKs
- retry standby sends
- perform blocking I/O for standby replication
- fsync
- perform heavy allocation
- block on unbounded mirror backpressure

Standby mirroring should leave the loop the same way the current bridge path
leaves the loop: as early and cheaply as possible.

---

## Mirror Worker

Mirroring should be handled outside the publisher hot path.

The mirror worker should:

- consume accepted frames from a bounded queue
- serialize frames for the standby peer
- send frames over a private standby mirror connection
- track standby lag
- reconnect or require fresh snapshots for standby peers as needed
- mark a standby stale if it cannot keep up

ACKs are not required for the first version unless they are needed to report
accurate freshness. A simpler v1 can report:

- latest accepted sequence on active
- latest enqueued mirror sequence
- latest sent mirror sequence
- latest applied mirror sequence if standby sends progress updates

---

## Slow Standby Behaviour

A slow standby must not slow the active node.

The mirror path should be bounded and observable.

If a standby falls behind beyond policy:

- stop sending to that standby
- drop or reset the mirror connection
- mark the standby stale
- make the standby ineligible for normal promotion
- require a fresh snapshot before it can become promotion-eligible again

The active node should continue serving publishers normally.

This avoids a slowdown loop where standby backpressure harms the active path,
causing publishers to reconnect or retry, increasing load further.

---

## Snapshot / Checkpoint Support

Accepted-frame mirroring keeps standby state fresh, but snapshots are still
useful.

Snapshots support:

- standby bootstrap
- process restart recovery
- recovery after mirror lag overflow
- validation before promotion
- warm storage fallback

Snapshots may contain:

- last-value caches
- debounce windows
- coalesce windows
- smoothing / filter state
- config hash
- conditioning metadata
- accepted sequence markers
- emitted sequence markers, if needed

The snapshot format should include a durable header. At minimum:

```text
magic / format version
source node id
accepted_seq
emitted_seq, if needed
monoblok version
build hash
patchbay sha256
effective config sha256
payload size
payload checksum
```

`accepted_seq` is load-bearing. A snapshot with `accepted_seq=N` means all
accepted input frames up to and including `N` are represented in the restored
state. After installing that snapshot, B must resume the mirror stream at
`N + 1`.

When B installs a snapshot from A, it should retain the snapshot header as part
of its local HA state. That retained header is what B reports in `$MBLK.STATUS`
and what it uses to verify the mirror stream that follows.

Snapshots should be transferable over the control listener. This avoids
requiring shared storage for the simple two-node design.

The active node can serve a binary snapshot with a control request:

```sh
nats --server nats://monoblok-a:8222 request '$MBLK.SNAPSHOT.GET' '{}'
```

When B is recovering, it should normally pull this snapshot from A and install
it directly in-process. No separate shared storage is required.

The standby may also install a binary snapshot through its own control listener
for operator-driven restore:

```sh
nats --server nats://monoblok-b:8222 request '$MBLK.SNAPSHOT.PUT' @snapshot.bin
```

The snapshot response/request body is the existing binary snapshot format, not
JSON. Snapshot metadata should either be embedded in the binary snapshot header
or returned in a small JSON envelope before a streaming mode is added.

Required snapshot metadata:

- accepted sequence
- patchbay sha256
- build hash
- snapshot format version
- whether downstream output was enabled when captured
- optional source node id

In this design, "resync" means installing a fresh snapshot and then replaying
any accepted frames after the snapshot's `accepted_seq`.

Sequencing invariant:

```text
snapshot.accepted_seq = N
first frame applied after restore = N + 1
```

B may already have mirrored frames `> N` buffered locally by the time the
snapshot finishes installing. That is fine. The snapshot establishes the cut;
the buffer supplies the tail.

B must reject promotion if it detects a mirror gap:

```text
expected accepted_seq=N+1
received accepted_seq>N+1
```

B may ignore or reject duplicates:

```text
received accepted_seq<=N
```

The policy can be strict, but the observable state must be clear: a gap means B
is stale and needs a fresh snapshot.

For small snapshots, `$MBLK.SNAPSHOT.GET` may return the snapshot as a
single response payload. For larger snapshots, the same subject should become a
chunked transfer: the request includes a reply inbox, and A sends snapshot
metadata, ordered binary chunks, and a final completion frame to that inbox. The
same chunk format can be used by `$MBLK.SNAPSHOT.PUT` when an operator
pushes a snapshot into a recovering node.

No sentinel is needed for binary snapshot bytes. Core NATS `PUB` frames already
carry an explicit payload length:

```text
PUB <reply-inbox> <byte-count>\r\n
<exactly byte-count bytes>\r\n
```

Snapshot chunks should therefore be ordinary NATS payloads. The chunk envelope
only needs enough metadata to order and validate chunks, for example:

```text
snapshot id
snapshot sequence
chunk index
chunk count or final flag
payload byte count
checksum, optional
```

Avoid sentinel-terminated binary streams. Snapshot data may contain any byte
sequence, and sentinel scanning would add complexity without improving framing.

Recommended bootstrap / fresh snapshot flow:

```text
A streams first.
B buffers.
A snapshots.
B restores snapshot.
B replays buffered frames over the restored state.
```

```text
B starts
B enters RECOVERING
B opens the mirror stream to A
B receives a mirror hello/header from A
B begins buffering mirrored frames without applying them
B requests a binary snapshot from A's control listener
B streams the snapshot chunks into its local snapshot installer
B verifies source/build/patchbay identity
B reads snapshot.accepted_seq=N from the installed snapshot header
B discards buffered mirrored frames with accepted_seq<=N
B replays buffered mirrored frames from N+1
B continues applying live mirror frames
B catches up
B marks itself STANDBY
```

The mirror stream should begin with a small header from A before any frames:

```json
{
  "source_node": "monoblok-a",
  "monoblok_version": "0.1.0",
  "build_hash": "def456",
  "patchbay_sha256": "abc123",
  "effective_config_sha256": "789abc",
  "latest_accepted_seq": 9321
}
```

B compares this header against its local identity before requesting or accepting
a snapshot. If the identity fields differ, B must reject the stream and remain
`RECOVERING` or `STALE`.

The snapshot then establishes the replay cut:

```text
snapshot.accepted_seq=N
drop buffered frames <= N
apply buffered/live frames starting at N+1
```

This avoids asking A to keep a special catch-up window after the snapshot. B is
already buffering the post-cut stream while the snapshot is being produced and
transferred.

### Active Behaviour During Lazy Bootstrap

When B appears lazily, A is already healthy and serving publishers. A should not
stand down and should not reject publisher traffic just because a standby is
bootstrapping.

Serving `$MBLK.SNAPSHOT.GET` may require a short capture barrier on A's loop
thread so the snapshot has a precise `accepted_seq`.

Acceptable v1 behavior:

```text
B opens mirror stream and starts buffering accepted frames
B requests snapshot
A reaches a safe point on the loop thread
A records accepted_seq=N
A copies LVC and rule state into a private snapshot arena
A resumes normal publisher processing
A streams or writes the snapshot from the private copy
B installs snapshot N
B drops buffered frames <= N
B replays buffered frames > N and catches up
```

During the capture barrier, A may briefly stop processing new publisher frames
on the loop. It should not close publisher connections, reject publishers, or
change active mode. Existing socket buffers and client-side buffers absorb the
short pause.

This is the same shape as periodic snapshots: copy state on the loop thread,
then do slower I/O from private memory. The difference is that a lazy standby
snapshot also establishes the exact replay cut for B's already-buffered mirror
stream.

If the snapshot copy takes too long or B's local mirror buffer overflows, B
should abandon the bootstrap attempt and request a fresh snapshot. A should keep
serving publishers.

### Shared Snapshot Storage

Direct snapshot transfer over the control listener is the simplest two-node
path. Shared snapshot storage is still useful when snapshots are large, when the
standby restarts while the active is busy, or when operators want an independent
warm recovery point.

Possible storage backends:

- shared filesystem volume
- network block or file storage mounted by both nodes
- S3-compatible object storage
- cloud blob storage
- JetStream Object Store, if a NATS deployment already exists

The storage object should be treated as a binary snapshot plus metadata. The
metadata may live in the snapshot header, in a sidecar object, or both.

Useful object metadata:

```text
ha group
source node id
snapshot sequence
created timestamp
monoblok version
build hash
patchbay sha256
effective config sha256
snapshot format version
payload size
payload checksum
```

For object stores, write snapshots with a publish-then-promote pattern:

```text
write snapshot object under a unique immutable key
verify checksum / size
write or update a small "latest" pointer object
```

Example key shape:

```text
monoblok/<group>/snapshots/<sequence>-<source-node>.mbsnap
monoblok/<group>/snapshots/latest.json
```

Avoid overwriting the latest binary snapshot in place. A standby should either
read a fully written immutable object or keep using its existing state.

Shared storage recovery flow:

```text
B starts in RECOVERING
B reads latest snapshot metadata from shared storage
B checks patchbay/build/snapshot format identity
B downloads and verifies the binary snapshot
B installs the snapshot locally
B connects to A mirror stream from snapshot.accepted_seq + 1
B catches up and becomes STANDBY
```

Shared storage does not replace the mirror stream. The snapshot is a base image;
the mirror stream still supplies accepted frames after `snapshot.accepted_seq`.

Shared storage also does not authorize promotion. It only provides bytes. The
operator or a future lease/epoch authority still decides which node may become
active.

---

## Version, Config, and Divergence Checks

A standby should not become promotion-eligible unless it can prove that it is
running the same behavior as the active node.

Minimum identity checks:

- monoblok binary version string
- monoblok build hash or executable digest, if available
- snapshot format version
- patchbay file byte hash
- effective runtime flags that affect conditioning behavior
- optional kernel version hash for `lib/patchbay/src/kernel.zig`

The patchbay hash should be over the exact `patchbay.edn` bytes loaded by the
process, not over a parsed or pretty-printed representation. Byte identity keeps
the rule simple: if A and B did not load the same file content, B is not
promotion-eligible.

Example status fields:

```json
{
  "monoblok_version": "0.1.0",
  "build_hash": "def456",
  "snapshot_format": 1,
  "patchbay_sha256": "abc123",
  "effective_config_sha256": "789abc"
}
```

The active should include these identity fields in:

- `$MBLK.STATUS`
- mirror stream hello / setup
- snapshot metadata

The standby should compare them before:

- accepting a snapshot
- applying mirror frames
- reporting `promotion_eligible=true`
- accepting `PROMOTE`

### Periodic State Checkpoints

A and B can occasionally compare compact state digests to detect divergence
while the standby is still warm.

The digest should cover state that affects future output:

- LVC entries, if LVC is enabled
- rule state tables
- debounce / coalesce windows
- smoothing / filter state
- latest accepted sequence applied
- relevant stats only if they affect behavior

The digest should not include incidental process-local details such as pointer
addresses, allocation order, connection ids, timestamps that do not affect
future behavior, or counters that are purely observational.

Suggested flow:

```text
A periodically computes state_digest at accepted_seq=N
B periodically computes state_digest at applied_seq=N
B reports the latest comparable digest through $MBLK.STATUS
A or an operator compares digests for the same sequence
if digests differ, B is marked STALE / diverged
```

This should be a background health check, not part of the publisher hot path.
Digesting can be amortized or triggered on a timer. If full state hashing is too
expensive, start with a lower-frequency checkpoint or hash only the serialized
snapshot representation.

Example status fields:

```json
{
  "state_digest_seq": 9354,
  "state_digest_sha256": "f00d42",
  "state_digest_compared": true,
  "state_diverged": false
}
```

If divergence is detected:

```text
B reports state_diverged=true
B reports promotion_eligible=false
B requires a fresh snapshot from A
```

The first implementation can compute the digest by serializing the same state
used for snapshots and hashing those bytes. That makes the checkpoint logic
boring and keeps it aligned with restore semantics.

---

## Failover Gap

This design does not guarantee zero-loss failover.

The possible crash gap is:

```text
last accepted frame mirrored to standby
    -> active node crash
```

Possible effects:

- a small amount of accepted input may not reach standby
- debounce / coalesce state may be slightly stale
- smoothing state may jump slightly
- duplicate downstream emission may occur around promotion
- publisher messages in flight to the crashed active may need to be retried by clients

The design goal is bounded, observable recovery, not perfectly synchronous HA.

A future stronger mode could require the active node to wait for standby ACKs
before considering a frame accepted, but that would allow standby health or
latency to affect the active path.

---

## Status Payload

Example control response for `$MBLK.STATUS`:

```json
{
  "node": "monoblok-b",
  "mode": "STANDBY",
  "publish_enabled": false,
  "output_enabled": false,
  "active_peer": "monoblok-a",
  "standby_peer": null,
  "monoblok_version": "0.1.0",
  "build_hash": "def456",
  "snapshot_format": 1,
  "patchbay_sha256": "abc123",
  "effective_config_sha256": "789abc",
  "latest_snapshot_seq": 9321,
  "latest_mirror_seq": 9354,
  "latest_applied_seq": 9354,
  "mirror_lag_frames": 0,
  "mirror_lag_ms": 12,
  "state_digest_seq": 9354,
  "state_digest_sha256": "f00d42",
  "state_diverged": false,
  "promotion_eligible": true,
  "reason": "standby_ready"
}
```

For publisher traffic, the important state is `publish_enabled`.

Only the active node should report:

```json
{
  "publish_enabled": true,
  "output_enabled": true
}
```

---

## Observability

Because the control listener is NATS-shaped, operators can inspect much of the
HA state with ordinary request/reply tooling.

Useful direct probes:

```sh
nats --server nats://monoblok-a:8222 request '$MBLK.STATUS' '{}'
nats --server nats://monoblok-b:8222 request '$MBLK.STATUS' '{}'
```

The `$MBLK.*` namespace is also useful as an internal observability convention.
Nodes may publish low-rate lifecycle and health events on the control listener
for tools that are explicitly connected to the control port.

Possible event subjects:

```text
$MBLK.EVENT.STARTING
$MBLK.EVENT.STANDBY_READY
$MBLK.EVENT.ACTIVE_READY
$MBLK.EVENT.STAND_DOWN
$MBLK.EVENT.MIRROR_STALE
$MBLK.EVENT.STANDBY_DISCONNECTED
$MBLK.EVENT.SNAPSHOT_INSTALLED
$MBLK.EVENT.STATE_DIVERGED
$MBLK.EVENT.PROMOTED
```

These events are advisory. They are useful for logs, dashboards, and debugging,
but they do not authorize promotion and they should not be routed through the
patchbay.

The control listener should make this easy to tap without affecting publisher
traffic:

```sh
nats --server nats://monoblok-a:8222 sub '$MBLK.EVENT.>'
```

Control-plane LVC is useful here. A tool that connects after a transition should
be able to ask for the latest known state without waiting for the next event.
This should be a separate LVC owned by the control listener, not the data
listener's normal `$LVC.*` cache.

Useful retained subjects:

```text
$MBLK.EVENT.ACTIVE_READY
$MBLK.EVENT.STANDBY_READY
$MBLK.EVENT.MIRROR_STALE
$MBLK.EVENT.STANDBY_DISCONNECTED
$MBLK.EVENT.STATE_DIVERGED
$MBLK.STATUS.CURRENT
```

The retained values should be small JSON status/event payloads. Binary snapshot
payloads must not be retained in the control LVC.

Event subscriptions are read-only. They may be allowed without the shared
secret if the deployment is comfortable exposing mode changes, lag, and
divergence signals to that network. Any request/reply subject, including
`$MBLK.STATUS`, should still require the token.

For very small deployments, repeated `$MBLK.STATUS` requests may be enough. A
separate event stream is useful once operators want to see transitions without
polling.

---

## Failure Cases

### Planned switch

```text
A receives STAND_DOWN control request
A rejects new publishers
A closes existing publisher connections
A stops downstream output
A enters STAND_DOWN
B receives PROMOTE control request
B validates promotion eligibility
B becomes ACTIVE
publishers reconnect to B
```

### Active crashes

```text
A stops responding
operator confirms or fences A
operator checks B status
operator force-promotes B
B becomes ACTIVE if validation passes or override is explicit
publishers reconnect to B
```

### Standby is slow

```text
A mirror queue fills or lag exceeds threshold
A drops B mirror connection
B is marked STALE
B cannot promote normally until it installs a fresh snapshot and catches up
A continues serving publishers
```

### Standby goes away

```text
B process exits, host fails, or mirror connection drops
A marks standby_peer unavailable
A drops queued mirror frames for B after bounded buffering is exhausted
A reports standby_connected=false
A reports promotion redundancy unavailable
A continues accepting publishers and emitting downstream output
```

Losing B must not cause A to stand down. The system is no longer redundant, but
the active writer remains correct.

Operator-visible status on A should make the degraded state obvious:

```json
{
  "mode": "ACTIVE",
  "publish_enabled": true,
  "output_enabled": true,
  "standby_connected": false,
  "standby_fresh": false,
  "promotion_redundancy": false,
  "reason": "standby_disconnected"
}
```

### Standby comes back

```text
B starts in RECOVERING
B rejects publisher traffic
B keeps downstream output disabled
B asks A for status over A's control listener
B requests a binary snapshot from A with $MBLK.SNAPSHOT.GET
B installs the snapshot locally with $MBLK.SNAPSHOT.PUT
B opens or resumes the mirror stream from snapshot.accepted_seq + 1
B applies mirrored frames until caught up
B reports mode=STANDBY and promotion_eligible=true
```

B must not assume its local state is fresh after a restart. It should either:

- validate that its local snapshot is current enough and catch up from A; or
- discard local warm state and install a fresh binary snapshot from A.

The fresh snapshot path should be the default because it is easier to reason
about operationally.

### Stale active returns

In v1 there is no automatic lease fencing. A stale active returning is an
operational hazard that must be handled by deployment policy.

Recommended behavior:

```text
A starts in RECOVERING or STANDBY, not ACTIVE
A must be explicitly promoted before accepting publishers
A rejects publisher traffic until promoted
```

This implies the configured initial mode matters. A node should not blindly
restart as active after an emergency failover unless the operator or deployment
system intentionally starts it that way.

---

## Future Automatic HA Layer

Automatic failover can be added later without changing the core mirror model.

Future automatic promotion would need shared authority, for example:

- JetStream KV for lease / epoch state
- an external coordinator
- deployment-platform leader election

At that point, promotion rules should require:

- active lease expired or released
- standby state fresh enough
- patchbay and binary identity match
- compare-and-swap promotion succeeds
- promoted epoch fences stale nodes

A single third monoblok as coordinator is not sufficient unless the operator
accepts it as a single point of failure. Making the coordinator itself highly
available reintroduces consensus, which this v1 design intentionally avoids.

---

## Open Questions

- Exact mirror frame binary format.
- Whether standby progress ACKs are needed for v1 status.
- Snapshot storage location: local disk, shared disk, or object store.
- Maximum acceptable mirror lag for normal promotion.
- Whether emitted output should be mirrored for shadow-output comparison.
- Whether `PROMOTE` should require explicit expected patchbay/build hashes.
- Whether periodic state digests should hash snapshot bytes or a cheaper
  canonical state summary.
- How much control protocol should be shared with normal NATS parsing.
- Whether shared-secret control auth is sufficient for the first implementation.

---

## Summary

The proposed v1 HA model is:

```text
publishers know A and B
+
one active data endpoint
+
one standby that rejects publishers
+
direct async accepted-frame mirroring from active to standby
+
manual stand-down / promote through a NATS-shaped control listener
```

This keeps the single-writer invariant intact, keeps standby replication cheap,
and gives operators an understandable manual switch path. Automatic failover can
be layered on later with a real lease / epoch authority, but it is not required
for warm standby.
