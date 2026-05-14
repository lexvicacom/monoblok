#!/usr/bin/env python3
"""End-to-end queue-group test.

Verifies:
  1. Three subscribers in the same queue group share delivery: each gets
     ~N/3 of N publishes (round-robin cursor in router.zig).
  2. A plain (no-queue) subscriber on the same subject gets all N (queue
     groups don't steal from plain fan-out).
  3. Two queue groups on the same subject act independently: each group
     delivers once per publish.
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "zig-out" / "bin" / "monoblok"
PORT = int(os.environ.get("PORT", "14777"))


def read_line(sock: socket.socket, buf: bytearray) -> bytes:
    while b"\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("peer closed before line")
        buf.extend(chunk)
    idx = buf.index(b"\r\n")
    line = bytes(buf[:idx])
    del buf[: idx + 2]
    return line


def connect() -> tuple[socket.socket, bytearray]:
    s = socket.create_connection(("127.0.0.1", PORT), timeout=2.0)
    s.settimeout(2.0)
    buf = bytearray()
    read_line(s, buf)  # drain INFO
    return s, buf


def count_msgs(
    sock: socket.socket,
    buf: bytearray,
    sid: bytes,
    want_total: int,
    timeout: float = 2.0,
) -> int:
    """Count MSG frames whose sid matches; consume payloads. Returns early on timeout."""
    n = 0
    deadline = time.monotonic() + timeout
    while n < want_total:
        sock.settimeout(max(0.05, deadline - time.monotonic()))
        try:
            line = read_line(sock, buf)
        except (TimeoutError, socket.timeout):
            return n
        if not line.startswith(b"MSG "):
            continue
        parts = line.split()
        sid_got = parts[2]
        nbytes = int(parts[-1])
        while len(buf) < nbytes + 2:
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError("peer closed mid-payload")
            buf.extend(chunk)
        del buf[: nbytes + 2]
        if sid_got == sid:
            n += 1
    return n


def main() -> int:
    if not BIN.exists():
        subprocess.check_call(["zig", "build"], cwd=ROOT)

    tmp = Path(tempfile.mkdtemp(prefix="monoblok-qg-py."))
    keep = os.environ.get("KEEP") == "1"
    failures: list[str] = []

    pb = tmp / "p.edn"
    pb.write_text("")  # no rules; plain pub/sub
    log_path = tmp / "monoblok.log"

    with open(log_path, "w") as lf:
        proc = subprocess.Popen(
            [str(BIN), "--port", str(PORT), "--patchbay", str(pb), "--no-lvc"],
            stdout=lf,
            stderr=lf,
        )
    time.sleep(0.3)

    try:
        # Test 1: three subs in group "workers" + one plain sub.
        a, abuf = connect()
        b, bbuf = connect()
        c, cbuf = connect()
        d, dbuf = connect()
        pub, _ = connect()

        a.sendall(b"SUB foo workers 100\r\n")
        b.sendall(b"SUB foo workers 200\r\n")
        c.sendall(b"SUB foo workers 300\r\n")
        d.sendall(b"SUB foo 400\r\n")
        time.sleep(0.1)

        # N=100 across 3 subs: round-robin gives exactly {34, 33, 33}.
        # Using a non-multiple of 3 here is deliberate (the remainder lands
        # on whichever sub the cursor hits first).
        N = 100
        for i in range(N):
            body = f"m{i}".encode()
            pub.sendall(b"PUB foo " + str(len(body)).encode() + b"\r\n" + body + b"\r\n")

        na = count_msgs(a, abuf, b"100", N, timeout=2.0)
        nb = count_msgs(b, bbuf, b"200", N, timeout=2.0)
        nc = count_msgs(c, cbuf, b"300", N, timeout=2.0)
        nd = count_msgs(d, dbuf, b"400", N, timeout=2.0)

        print(f"test1 qg A (sid 100): {na}")
        print(f"test1 qg B (sid 200): {nb}")
        print(f"test1 qg C (sid 300): {nc}")
        print(f"test1 plain (sid 400): {nd}")

        if na + nb + nc != N:
            failures.append(f"test1: qg total {na + nb + nc} != {N}")
        if nd != N:
            failures.append(f"test1: plain sub got {nd}, want {N}")
        # Exact round-robin split: sorted counts must be [33, 33, 34].
        if sorted([na, nb, nc]) != [33, 33, 34]:
            failures.append(f"test1: split {sorted([na, nb, nc])} != [33, 33, 34]")

        for sock in (a, b, c, d, pub):
            sock.close()

        # Test 2: two independent queue groups on the same subject.
        # Each group delivers once per publish, so group totals == N each.
        x, xbuf = connect()
        y, ybuf = connect()
        z, zbuf = connect()
        w, wbuf = connect()
        pub2, _ = connect()

        x.sendall(b"SUB bar groupA 10\r\n")
        y.sendall(b"SUB bar groupA 20\r\n")
        z.sendall(b"SUB bar groupB 30\r\n")
        w.sendall(b"SUB bar groupB 40\r\n")
        time.sleep(0.1)

        M = 50
        for i in range(M):
            body = f"n{i}".encode()
            pub2.sendall(b"PUB bar " + str(len(body)).encode() + b"\r\n" + body + b"\r\n")

        nx = count_msgs(x, xbuf, b"10", M, timeout=2.0)
        ny = count_msgs(y, ybuf, b"20", M, timeout=2.0)
        nz = count_msgs(z, zbuf, b"30", M, timeout=2.0)
        nw = count_msgs(w, wbuf, b"40", M, timeout=2.0)

        print(f"test2 groupA: {nx} + {ny} = {nx + ny} (want {M})")
        print(f"test2 groupB: {nz} + {nw} = {nz + nw} (want {M})")

        if nx + ny != M:
            failures.append(f"test2: groupA total {nx + ny} != {M}")
        if nz + nw != M:
            failures.append(f"test2: groupB total {nz + nw} != {M}")

        for sock in (x, y, z, w, pub2):
            sock.close()

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            proc.kill()

        if keep:
            print(f"kept tmpdir: {tmp}")
        else:
            for p in tmp.iterdir():
                p.unlink()
            tmp.rmdir()

    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
