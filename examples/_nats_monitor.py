#!/usr/bin/env python3
import socket
import sys
import time
from urllib.parse import urlparse


HEADER_ORDER = {
    "x-monoblok": 0,
    "x-monoblok-replay": 1,
    "x-monoblok-assumed-ts": 2,
}


def clean_cell(raw):
    return raw.decode("utf-8", errors="replace").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")


def parse_url(raw_url):
    parsed = urlparse(raw_url if "://" in raw_url else f"nats://{raw_url}")
    return parsed.hostname or "127.0.0.1", parsed.port or 4222


class NatsMonitor:
    def __init__(self, url, subject):
        self.host, self.port = parse_url(url)
        self.subject = subject.encode("utf-8")
        self.sock = socket.create_connection((self.host, self.port), timeout=5)
        self.sock.settimeout(None)
        self.buf = b""
        self.started = time.monotonic()

    def read_more(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError
        self.buf += chunk

    def read_line(self):
        while b"\r\n" not in self.buf:
            self.read_more()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line.decode("ascii", errors="replace")

    def read_bytes(self, length):
        while len(self.buf) < length + 2:
            self.read_more()
        data = self.buf[:length]
        trailer = self.buf[length:length + 2]
        self.buf = self.buf[length + 2:]
        if trailer != b"\r\n":
            raise RuntimeError("invalid NATS payload trailer")
        return data

    def subscribe(self):
        self.read_line()
        self.sock.sendall(b'CONNECT {"headers":true}\r\n')
        self.sock.sendall(b"SUB " + self.subject + b" 1\r\n")
        self.sock.sendall(b"PING\r\n")

    def emit_msg(self, parts):
        size = int(parts[-1])
        payload = self.read_bytes(size)
        self.print_row(parts[1], payload, [])

    def emit_hmsg(self, parts):
        header_len = int(parts[-2])
        total_len = int(parts[-1])
        frame = self.read_bytes(total_len)
        header_frame = frame[:header_len]
        payload = frame[header_len:]
        self.print_row(parts[1], payload, parse_headers(header_frame))

    def print_row(self, subject, payload, headers):
        elapsed = f"{time.monotonic() - self.started:.6f}s"
        cells = [elapsed, subject, clean_cell(payload)]
        if headers:
            cells.append("; ".join(f"{name}={value}" for name, value in headers))
        print("\t".join(cells), flush=True)

    def run(self):
        self.subscribe()
        while True:
            line = self.read_line()
            if not line or line == "PONG" or line == "+OK" or line.startswith("INFO "):
                continue
            if line.startswith("-ERR"):
                print(line, flush=True)
                continue
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "MSG" and len(parts) >= 4:
                self.emit_msg(parts)
            elif parts[0] == "HMSG" and len(parts) >= 5:
                self.emit_hmsg(parts)
            elif parts[0] == "PING":
                self.sock.sendall(b"PONG\r\n")


def parse_headers(raw):
    lines = raw.decode("utf-8", errors="replace").split("\r\n")
    headers = []
    for line in lines[1:]:
        if not line:
            continue
        name, sep, value = line.partition(":")
        if sep:
            headers.append((name.strip(), value.strip()))
    return sorted(headers, key=lambda item: (HEADER_ORDER.get(item[0].lower(), 100), item[0].lower()))


def main():
    if len(sys.argv) != 3:
        print("usage: _nats_monitor.py URL SUBJECT", file=sys.stderr)
        return 2
    try:
        NatsMonitor(sys.argv[1], sys.argv[2]).run()
    except (BrokenPipeError, EOFError, KeyboardInterrupt):
        return 0


if __name__ == "__main__":
    sys.exit(main())
