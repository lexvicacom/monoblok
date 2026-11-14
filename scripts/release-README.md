# monoblok release archive

This archive contains a prebuilt `monoblok` binary, runnable patchbay examples,
and the benchmark helpers.

## Runtime requirements

The binary requires OpenSSL 3 runtime libraries. On macOS, install them first:

```sh
brew install openssl@3
```

On Linux, install your distribution's OpenSSL 3 runtime package.

The `linux-armv6-gnueabihf` archive is for 32-bit ARM hard-float Linux,
including Raspberry Pi 1 and Pi Zero W. It requires glibc 2.36 or newer,
such as Raspberry Pi OS Bookworm or newer. Use `linux-aarch64` on a 64-bit OS.

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

