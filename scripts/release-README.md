# monoblok release archive

This archive contains a prebuilt `monoblok` binary, runnable patchbay examples,
and the benchmark helpers.

## Quick start

Run the bundled tour patchbay:

```sh
./monoblok --patchbay patchbay.edn
```

Or run an example:

```sh
./monoblok --patchbay examples/demo.edn
```

By default monoblok listens on `127.0.0.1:4222`. Use `--host` and `--port` to
change that.

Useful checks:

```sh
./monoblok --validate examples/demo.edn
printf 'sensors.temp|31\n' | ./monoblok --soundcheck examples/sensors.edn
```

## Benchmarks

The benchmark scripts require the NATS CLI on `PATH`.

```sh
./bench.sh
./bench-with-nats-server.sh
```

On Linux, the benchmark scripts run monoblok with libuv io_uring by default.
Pass `--epoll` to compare against the production-default epoll path.

## Linux service install

Linux archives include `monoblok.service` and `install-systemd.sh`.

```sh
sudo ./install-systemd.sh
```

The installer expects this unpacked archive layout.

