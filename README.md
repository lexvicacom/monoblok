# monoblok

Experimental toy NATS-compatible pub/sub daemon (PUB, SUB, and the other
basics) with S-expression routing rules and last-value streams. Single
event loop on [libxev](https://github.com/mitchellh/libxev).

## Build & run

```
zig build --release=fast
./zig-out/bin/monoblok --port 4222 --rules rules.edn
```

Any NATS client works:

```
nats -s nats://127.0.0.1:4222 pub sensors.temp 42.5
nats -s nats://127.0.0.1:4222 sub 'sensors.*'
```

Subjects are alphanumerics plus `- _ $`; `*` is a single token, `>` is
a trailing wildcard. Core protocol only — no auth, TLS, queue groups,
headers, JetStream.

## Rules

Top-level forms: `(on SUBJECT-FILTER BODY)`. Bound symbols: `subject`,
`payload`, `payload-float`. Forms: `publish`, `subject-append`,
`str-concat`, `contains?`, `if` / `when` / `and` / `or` / `not`,
`= < <= > >= + - * /`. Rule-published messages fan out normally but do
not re-enter the rule engine. See `rules.edn`.

## `$LVC.*` — last-value stream

Every subject has an implicit last-value cache. Subscribing to
`$LVC.foo.bar` joins a live stream of `foo.bar`: current cached value
first (if any), then every subsequent publish. Wildcards work.
`PUB $LVC.*` is rejected.

```
PUB foo.bar 11      ; cache = 11
PUB foo.bar 12      ; cache = 12
SUB $LVC.foo.bar    ; -> immediately receives 12
PUB foo.bar 13      ; -> subscriber receives 13
```

On by default; `--no-lvc` disables (~2–4% overhead when enabled).

## Architecture

One `xev.Loop` owns accept, per-connection read/write completions,
router state, and the LVC. No mutexes, no atomics on the hot path.
Fan-out appends bytes directly to each subscriber's outbound
`ArrayList` and kicks a single `write` per connection per publish,
with partial-write handling.

See `CLAUDE.md` for module-level detail.

## Tests

```
zig build test              # unit tests
bash scripts/smoke.sh       # end-to-end over raw TCP
bash scripts/bench.sh       # pub + fan-out bench (needs `nats` CLI)
```

## Benchmarks

Fair warning before the numbers: `nats-server` is a mature, battle-tested
Go codebase with a decade of production history behind it. It's doing
substantially more than monoblok — accounting, logging, metrics, slow-consumer
detection, account isolation, clustering, JetStream, TLS, auth — any of
which has a non-zero runtime cost even when unused. monoblok is a
comparative toy implementing a tiny slice of that. Treat these numbers as
informational, not as a claim of "faster than nats-server."

M2 MacBook Air (8-core, 16 GB), Zig 0.16, libxev kqueue backend,
vs `nats-server` 2.9.6 on the same machine. `nats bench` as the load
generator. Single run each — indicative, not rigorous. monoblok built
with `--release=safe` (what `zig build dist` produces).

Publish-only:

| workload | monoblok | nats-server |
|---|---:|---:|
| 1 × 500k × 64B | 6.12M msg/s | 6.18M msg/s |
| 2 × 10k × 64B | 4.57M msg/s | 5.19M msg/s |
| 8 × 50k × 128B | 4.64M msg/s | 10.29M msg/s |

Fan-out (1 pub, N subs, aggregated sub rate):

| subs | monoblok | nats-server |
|---|---:|---:|
| 1 | 2.04M msg/s | 2.99M msg/s |
| 10 | 7.03M msg/s | 6.76M msg/s |
| 50 | 8.01M msg/s | 6.70M msg/s |

Single-publisher parity; multi-publisher nats-server pulls ahead
(their sublist is a token-keyed radix tree, ours is linear); fan-out
we still edge out at 10+ subs. `--release=fast` gains roughly 10-15%
on fan-out if you want to see the ceiling.

### Linux

Ubuntu 24.04 (2-core AMD EPYC KVM VM, 4 GB), Zig 0.16, libxev
io_uring backend, vs `nats-server` v2.12.7 on the same machine.
ReleaseSafe (via `zig build dist`).

Publish-only:

| workload | monoblok | nats-server |
|---|---:|---:|
| 1 × 500k × 64B | 2.77M msg/s | 3.79M msg/s |
| 2 × 10k × 64B | 2.74M msg/s | 2.38M msg/s |
| 8 × 50k × 128B | 3.40M msg/s | 3.50M msg/s |

Fan-out:

| subs | monoblok | nats-server |
|---|---:|---:|
| 1 | 0.65M msg/s | 1.57M msg/s |
| 10 | 2.87M msg/s | 3.01M msg/s |
| 50 | 4.51M msg/s | 3.28M msg/s |

Numbers are lower than the M2 MBA as expected for a 2-vCPU cloud VM.
io_uring works cleanly end-to-end. Single-subscriber fan-out is weak —
the 1-sub workload is dominated by scheduling on a 2-vCPU box and our
profile differs from nats-server's there; ~10 subs onward it levels
out.

## Why libxev

Zig 0.16's `std.Io` networking backends are broken on every target we
tried: `Dispatch` (macOS) has no net ops, `Kqueue` references a vtable
field that no longer exists, `Uring` has error-set mismatches. libxev
has working kqueue/io_uring/epoll/IOCP, so that's what we use.

## Cross-compile

Zig cross-compiles out of the box. The `dist` build step produces
ReleaseSafe binaries for common targets into `dist/<triple>/`,
alongside `rules.edn` and the bench script:

```
zig build dist
# dist/x86_64-linux-musl/   → monoblok (static musl ELF)
# dist/aarch64-linux-musl/  → monoblok (static musl ELF, ARM64)
# dist/x86_64-windows-gnu/  → monoblok.exe
```

Pick one and `scp` it anywhere — the Linux binaries are statically
linked against musl so there's no glibc dependency.

For an ad-hoc one-off target that isn't in the dist set, the vanilla
Zig flag still works:

```
zig build --release=safe -Dtarget=x86_64-linux-gnu
```

libxev picks the right backend at comptime: `io_uring` on Linux,
`kqueue` on macOS, `iocp` on Windows. The daemon logs which backend
it's using at startup.

## Releases

Pushing a tag matching `v[0-9]*` (e.g. `v0.1.0`) triggers the release
workflow in `.github/workflows/release.yml`: it runs tests, runs
`zig build dist`, packages each target (`tar.gz` for Linux,
`zip` for Windows), and attaches them to a new GitHub release.

Only Linux (`x86_64`, `aarch64`) and Windows (`x86_64`) binaries are
shipped. macOS is not included — unsigned Mac binaries hit Gatekeeper
warnings and want an `xattr -d com.apple.quarantine` dance, which is
more friction than just building locally. If you're on a Mac:
`zig build --release=safe`.

## License

MIT. See `LICENSE`.
