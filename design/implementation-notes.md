# Implementation notes

## Patchbay arena allocator

Patchbay uses a small bump arena for AST nodes and short-lived eval values.
This is not meant to be clever. It fits because the lifetimes are obvious:
parsed patchbay data lives until the loaded program is freed, and eval scratch
lives until the outer publish or clock tick finishes.

Each arena block has a `used` cursor and a capacity. Allocation aligns the
cursor, returns a pointer inside the block, and bumps `used`. If no existing
block fits, the arena allocates another block and links it in. Individual
allocations are never freed. `pb_arena_reset` marks every block empty for
reuse, and `pb_arena_free` releases the lot.

That keeps ownership out of the parser and evaluator. Lists, vectors, parsed
text, coerced payload strings, JSON extracts, and temporary builtin output all
follow the same rule: the pointer is valid until the owning arena is reset or
freed. The code stays local, and normal publish handling avoids surprise heap
traffic.

The usual arena trap is retained high-water memory, so scratch arenas can be
trimmed after reset with `pb_arena_trim`. It frees unused blocks above a small
retained-cap budget. The parse arena does not need that because it is long-lived
program ownership. Scratch does: one large payload should not hold on to that
memory until daemon shutdown.

So the arena is deliberately narrow. It is not a general allocator. It is a
simple lifetime tool for patchbay-owned data and temporary publish work.

## Single-threaded core

The server core belongs to one libuv loop thread. Connections, subscriptions,
the router, LVC, patchbay state, write queues, and bridge fan-out are all
mutated there. The hot path avoids mutexes and atomics because ownership is
settled by the design, not argued over at each access.

The boundaries are kept plain. The router does not know about libuv handles; it
gets router-facing connection objects and appends output bytes. The server owns
sockets, connection lifetime, and write completion. Patchbay eval runs from
publish handling and can publish back through the router only through explicit
hooks.

The main exception is snapshot file I/O. Periodic snapshots are serialized into
owned bytes on the loop, then the blocking atomic write is queued to libuv's
worker pool. The worker gets only a copied path and a completed byte buffer. It
does not touch router or patchbay state. Startup load and shutdown write are
synchronous because they are outside the live publish path.

## libuv runtime

libuv gives us the event loop, TCP listener, timers, signal handling, worker
queue, and platform backend. On Linux that normally means epoll. On macOS and
the BSDs it means kqueue. Monoblok does not hide libuv behind another framework;
the server module is the boundary between transport callbacks and the routing
and protocol code.

On Linux, libuv can use optional io_uring paths. Monoblok disables those by
default before loop creation unless the environment already says otherwise.
That is an operational choice. Hardened containers and locked-down distros often
restrict io_uring through seccomp or kernel policy, so the default should work
in boring production environments. `--io-uring` or `UV_USE_IO_URING=1` opts back
in when the host is known to support it.

## Borrowed slices

`mb_slice` and `pb_slice` are borrowed views: pointer plus length, no ownership.
They avoid temporary C strings on protocol input, router matching, patchbay AST
text, JSON-derived values, and emitted payloads.

The rule is simple: a slice can be compared, parsed, or forwarded while its
source buffer is alive, but it must be duplicated before it is stored past that
lifetime. Protocol parsing returns slices into the connection receive buffer.
SUB stores duplicate subject, queue, and SID bytes in the router. Patchbay parse
text is copied into the parse arena. Temporary eval strings live in scratch
until the outer publish or tick ends.

This avoids hidden `strlen` scans and NUL-termination work in the common path.
When an outside API needs a C string, such as `nats.c` bridge publishing, the
bridge keeps reusable scratch storage and copies only the subject being handed
to that API.

## Allocation-free hot paths

The target is not "no allocation anywhere". The target is no surprising
allocation in the steady-state publish path.

Protocol parsing is allocation-free. The read callback reuses a per-connection
read buffer, the parser returns slices into the receive buffer, and consumed
bytes are compacted in place. `CONNECT`, `PING`, `PUB`, `SUB`, `UNSUB`, and
queue-group parsing do not allocate while parsing.

Routing is allocation-conscious rather than allocation-hostile. SUB/UNSUB,
connection open/close, first LVC subject creation, and index growth are allowed
to allocate. Once subscriptions, output buffers, queue scratch, and LVC payload
capacity are warm, ordinary publish fan-out is pointer walking, subject
matching, MSG serialization, and buffer appends. Queue-group selection uses
router-owned scratch arrays, not per-delivery heap objects.

Protocol output is staged in reusable connection buffers. MSG writing computes
the full frame length and reserves once before appending the prefix, subject,
SID, byte count, payload, and trailing CRLF. Each connection keeps one guarded
`uv_write_t`: queued bytes are swapped into `in_flight`, written, then the next
queued batch is submitted after completion.

Patchbay eval uses arena scratch for temporary values. Stateful forms may
allocate when a state slot is first created or when their owned buffers or rings
grow, but repeated steady-state eval reuses the same state and resets scratch as
a group. The scratch arena trim keeps that reuse from turning one large event
into permanent memory retention.
