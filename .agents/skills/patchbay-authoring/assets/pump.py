#!/usr/bin/env python
"""Publish randomized telemetry into a monoblok/NATS endpoint."""

from __future__ import annotations

import argparse
import json
import random
import socket
import time


class NatsPublisher:
    def __init__(self, host: str, port: int, name: str) -> None:
        self.sock = socket.create_connection((host, port), timeout=5.0)
        self.file = self.sock.makefile("rb", buffering=0)
        info = self.file.readline()
        if not info.startswith(b"INFO "):
            raise RuntimeError(f"unexpected NATS greeting: {info!r}")

        connect = json.dumps(
            {
                "verbose": False,
                "pedantic": False,
                "name": name,
                "lang": "python",
                "version": "0",
            },
            separators=(",", ":"),
        )
        self.sock.sendall(f"CONNECT {connect}\r\nPING\r\n".encode("ascii"))
        self._read_pong()

    def publish(self, subject: str, payload: str) -> None:
        body = payload.encode("utf-8")
        self.sock.sendall(f"PUB {subject} {len(body)}\r\n".encode("ascii"))
        self.sock.sendall(body + b"\r\n")

    def close(self) -> None:
        self.sock.close()

    def _read_pong(self) -> None:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            line = self.file.readline()
            if line == b"PONG\r\n":
                return
            if line.startswith(b"-ERR"):
                raise RuntimeError(line.decode("utf-8", "replace").strip())
        raise TimeoutError("NATS server did not answer PING")


def publish_json(pub: NatsPublisher, subject: str, value: dict[str, object]) -> None:
    pub.publish(subject, json.dumps(value, separators=(",", ":")))


def run(args: argparse.Namespace) -> None:
    random.seed(args.seed)
    pub = NatsPublisher(args.host, args.port, "patchbay-pump")
    sent = 0

    try:
        while args.count == 0 or sent < args.count:
            publish_json(
                pub,
                "example.device-01.telemetry",
                {
                    "temperature": round(random.uniform(18.0, 35.0), 1),
                    "pressure": round(random.uniform(980.0, 1030.0), 1),
                    "mode": random.choice(["AUTO", "AUTO", "MANUAL", "FAULT"]),
                },
            )
            sent += 1

            if args.verbose:
                print(f"published {sent} messages")

            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        pub.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Pump sample telemetry into monoblok.")
    parser.add_argument("--host", default="127.0.0.1", help="NATS host")
    parser.add_argument("--port", type=int, default=4222, help="NATS port")
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--count", type=int, default=0, help="0 runs until interrupted")
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
