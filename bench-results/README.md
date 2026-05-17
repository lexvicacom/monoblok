# Saved benchmark runs

These are saved script outputs used to check that monoblok is performing roughly
where expected on a few representative machines. They are useful for spotting
regressions and understanding workload shape. They are **not** broad performance
claims.

Files ending in `-vs-nats.txt` come from:

```sh
scripts/bench-with-nats-server.sh
```

Files ending in `-pb.txt` come from:

```sh
scripts/bench.sh
```

The saved hosts are:

- `hetzner-cax11`: Linux/aarch64, 2 cores, 4 GB.
- `hetzner-cpx42`: Linux/x86_64, 8 cores, 15 GB.
- `mac-mini-m4`: macOS/arm64, 10 cores, 16 GB.

Each output records the host, monoblok version, NATS CLI version, optional
`nats-server` version, libuv mode, workloads, and msgs/sec table. On Linux the
benchmark scripts default to monoblok's opt-in libuv io_uring path, matching the
saved Linux runs. Pass `--epoll` to compare the production-default epoll path on
your own hardware.
