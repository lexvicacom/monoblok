#!/usr/bin/env python
"""Mixer-mode example.

Starts a mixer with three worker shards (SENSORS, ORDERS, catch-all), each
with its own patchbay. The single client connects to the mixer and
publishes / subscribes as if it were one normal NATS server. The mixer
forwards each frame to the worker owning the first subject token, and
fans MSGs back out to subscribed clients.

Run from the repo root:
    python examples/mixer.py
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "zig-out" / "bin" / "monoblok"
PORT = int(os.environ.get("PORT", "14222"))
MIXER_EDN = ROOT / "examples" / "mixer.edn"


class Client:
    def __init__(self, sock: socket.socket, label: str) -> None:
        self.sock = sock
        self.sock.settimeout(2.0)
        self.label = label
        self.buf = bytearray()
        self.read_line()  # drain INFO

    @classmethod
    def connect(cls, label: str = "client") -> "Client":
        s = socket.create_connection(("127.0.0.1", PORT), timeout=2.0)
        return cls(s, label)

    def send(self, data: bytes) -> None:
        self.sock.sendall(data)

    def read_line(self) -> bytes:
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("peer closed")
            self.buf.extend(chunk)
        idx = self.buf.index(b"\r\n")
        line = bytes(self.buf[:idx])
        del self.buf[: idx + 2]
        return line

    def drain_msgs(self, duration: float = 0.4) -> list[tuple[str, str, str]]:
        """Read whatever MSG frames arrive within `duration`. Returns
        (subject, sid, payload) triples."""
        out: list[tuple[str, str, str]] = []
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.sock.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                line = self.read_line()
            except (TimeoutError, socket.timeout):
                break
            if not line.startswith(b"MSG "):
                continue
            parts = line.split()
            subject = parts[1].decode()
            sid = parts[2].decode()
            nbytes = int(parts[-1])
            while len(self.buf) < nbytes + 2:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return out
                self.buf.extend(chunk)
            payload = bytes(self.buf[:nbytes]).decode(errors="replace")
            del self.buf[: nbytes + 2]
            out.append((subject, sid, payload))
        return out

    def close(self) -> None:
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


def banner(text: str) -> None:
    print()
    print(text)
    print("-" * len(text))


def main() -> int:
    if not BIN.exists():
        subprocess.check_call(["zig", "build"], cwd=ROOT)

    print(f"starting mixer on port {PORT} (cfg: {MIXER_EDN})")
    proc = subprocess.Popen(
        [str(BIN), "--mixer", str(MIXER_EDN)],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        # Wait for listener.
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.1).close()
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("mixer did not start")

        sub = Client.connect("subscriber")
        # Subscribe to one subject from each shard. SENSORS.>.stable picks
        # up the conditioned mirror; ORDERS.audit picks up the rule's
        # synthesised topic; LOGS.> falls through to the catch-all.
        sub.send(b"SUB SENSORS.> 1\r\n")
        sub.send(b"SUB ORDERS.> 2\r\n")
        sub.send(b"SUB LOGS.> 3\r\n")
        time.sleep(0.05)

        pub = Client.connect("publisher")

        banner("publish into the SENSORS shard (round + squelch)")
        for v in ("42.01", "42.04", "42.08", "42.12", "43.00", "43.00", "43.01"):
            line = f"PUB SENSORS.temp {len(v)}\r\n{v}\r\n".encode()
            pub.send(line)
            print(f"  -> SENSORS.temp = {v}")

        banner("publish into the ORDERS shard (audit on 'alert')")
        for s, p in [
            ("ORDERS.42", "ok"),
            ("ORDERS.42", "alert: payment failed"),
            ("ORDERS.99", "ok"),
        ]:
            line = f"PUB {s} {len(p)}\r\n{p}\r\n".encode()
            pub.send(line)
            print(f"  -> {s} = {p!r}")

        banner("publish into the catch-all shard (no rules, just delivery)")
        for s, p in [("LOGS.app", "started"), ("LOGS.app", "ready")]:
            line = f"PUB {s} {len(p)}\r\n{p}\r\n".encode()
            pub.send(line)
            print(f"  -> {s} = {p!r}")

        # Give workers + mixer time to process.
        time.sleep(0.4)
        msgs = sub.drain_msgs(0.4)

        sub.close()
        pub.close()

        banner(f"subscriber received {len(msgs)} MSG(s)")
        for subject, sid, payload in msgs:
            print(f"  sid={sid}  {subject:30s}  {payload!r}")

        banner("notes")
        print("  * SENSORS.temp.stable was synthesized by the SENSORS worker's")
        print("    rule (round 1 + squelch). The 7 inbound publishes collapse")
        print("    to a handful of stable values.")
        print("  * SENSORS.temp itself is also delivered (the inbound publish")
        print("    fans out before the rule emits the .stable mirror).")
        print("  * ORDERS.audit was synthesized only when the payload contained")
        print("    'alert'; ordinary OK orders pass through but produce no audit.")
        print("  * LOGS.* hit the catch-all worker, which has no conditioning.")
        print("  * The subscriber connected ONCE to the mixer and got messages")
        print("    from all three shards (separate processes) transparently.")

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    return 0


if __name__ == "__main__":
    sys.exit(main())
