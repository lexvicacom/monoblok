#!/usr/bin/env python3
"""Publish NATS messages at a steady rate for long memory observations."""

from __future__ import annotations

import argparse
import math
import os
import signal
import socket
import sys
import time
from urllib.parse import urlparse


STOP = False


def request_stop(_signum: int, _frame: object) -> None:
    global STOP
    STOP = True


def parse_nats_url(value: str) -> tuple[str, int]:
    parsed = urlparse(value if "://" in value else f"nats://{value}")
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 4222
    return host, port


def make_payload(
    counter: int,
    payload_size: int,
    fixed_payload: bytes | None,
    demo_payloads: bool,
) -> bytes:
    if fixed_payload is not None:
        return fixed_payload

    if demo_payloads:
        # Numeric payloads keep examples/demo.edn's payload-float rules active.
        value = 35.0 + (20.0 * math.sin(counter / 20.0))
        return f"{value:.2f}".encode("ascii")

    base = f"{counter} {time.time_ns()}".encode("ascii")
    if payload_size <= 0 or len(base) >= payload_size:
        return base
    return base + (b"." * (payload_size - len(base)))


def connect(host: str, port: int, timeout_s: float) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=timeout_s)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.sendall(b"CONNECT {}\r\n")
    return sock


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be > 0")
    return parsed


def non_negative_float(value: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return parsed


def parse_args() -> argparse.Namespace:
    nats_url = os.environ.get("NATS_URL", "nats://demo.rtd.pub:4222")
    default_host, default_port = parse_nats_url(nats_url)

    parser = argparse.ArgumentParser(
        description=(
            "Publish raw NATS PUB frames at a steady rate. Defaults are chosen "
            "for long-running monoblok memory observation."
        )
    )
    parser.add_argument("--host", default=os.environ.get("HOST", default_host))
    parser.add_argument(
        "--port",
        type=int,
        default=int(os.environ.get("PORT", default_port)),
    )
    parser.add_argument(
        "--subject",
        default=os.environ.get("SUBJECT", "demo.sensors.temp"),
    )
    parser.add_argument(
        "--rate",
        type=positive_float,
        default=float(os.environ.get("RATE", "100")),
        help="messages per second",
    )
    parser.add_argument(
        "--duration",
        type=non_negative_float,
        default=float(os.environ.get("DURATION", "0")),
        help="seconds to run; 0 means until interrupted",
    )
    parser.add_argument(
        "--payload-size",
        type=int,
        default=int(os.environ.get("PAYLOAD_SIZE", "64")),
        help="generated payload bytes for --raw-payloads; ignored by demo defaults and --payload",
    )
    parser.add_argument(
        "--payload",
        default=os.environ.get("PAYLOAD"),
        help="fixed payload to publish every time",
    )
    parser.add_argument(
        "--raw-payloads",
        action="store_true",
        default=os.environ.get("RAW_PAYLOADS", "") not in ("", "0", "false", "False"),
        help="send padded counter/timestamp payloads instead of demo.edn numeric payloads",
    )
    parser.add_argument(
        "--report-every",
        type=positive_float,
        default=float(os.environ.get("REPORT_EVERY", "10")),
        help="seconds between progress reports",
    )
    parser.add_argument(
        "--connect-timeout",
        type=positive_float,
        default=float(os.environ.get("CONNECT_TIMEOUT", "5")),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    fixed_payload = args.payload.encode("utf-8") if args.payload is not None else None
    interval_s = 1.0 / args.rate

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    sock = connect(args.host, args.port, args.connect_timeout)
    started_s = time.monotonic()
    next_send_s = started_s
    next_report_s = started_s + args.report_every
    sent = 0
    subject = args.subject.encode("ascii")

    print(
        f"publishing {args.rate:g}/s to {args.host}:{args.port} subject={args.subject}",
        file=sys.stderr,
        flush=True,
    )

    try:
        while not STOP:
            now_s = time.monotonic()
            if args.duration > 0 and now_s - started_s >= args.duration:
                break

            sleep_s = next_send_s - now_s
            if sleep_s > 0:
                time.sleep(sleep_s)
                now_s = time.monotonic()

            sent += 1
            payload = make_payload(sent, args.payload_size, fixed_payload, not args.raw_payloads)
            frame = b"PUB " + subject + b" " + str(len(payload)).encode("ascii") + b"\r\n" + payload + b"\r\n"
            sock.sendall(frame)

            next_send_s += interval_s
            if next_send_s < now_s - interval_s:
                next_send_s = now_s + interval_s

            if now_s >= next_report_s:
                elapsed_s = max(now_s - started_s, 0.001)
                print(
                    f"sent={sent} elapsed={elapsed_s:.1f}s avg_rate={sent / elapsed_s:.1f}/s",
                    file=sys.stderr,
                    flush=True,
                )
                next_report_s = now_s + args.report_every
    finally:
        sock.close()

    elapsed_s = max(time.monotonic() - started_s, 0.001)
    print(
        f"done sent={sent} elapsed={elapsed_s:.1f}s avg_rate={sent / elapsed_s:.1f}/s",
        file=sys.stderr,
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
