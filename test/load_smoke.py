#!/usr/bin/env python3
"""Socket-level load smoke for exact fan-out and patchbay numeric delivery."""

from __future__ import annotations

import argparse
import math
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


SID_RAW = "1"
SID_AVG4 = "2"
SID_SUM8 = "3"
SID_COUNT = "4"

PATCHBAY = """\
(on "load.*"
  (do
    (publish! (subject-append "avg4") (moving-avg 4 payload-float))
    (publish! (subject-append "sum8") (moving-sum 8 payload-float))
    (count!)))
"""


class ProtocolError(RuntimeError):
    pass


class ProtoReader:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.buf = bytearray()

    def _recv_more(self, deadline: float) -> None:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for protocol data")
            self.sock.settimeout(min(1.0, remaining))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                raise EOFError("socket closed")
            self.buf.extend(chunk)
            return

    def read_line(self, deadline: float) -> bytes:
        while True:
            idx = self.buf.find(b"\n")
            if idx >= 0:
                out = bytes(self.buf[: idx + 1])
                del self.buf[: idx + 1]
                return out
            self._recv_more(deadline)

    def read_exact(self, n: int, deadline: float) -> bytes:
        while len(self.buf) < n:
            self._recv_more(deadline)
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out


def read_server_op(reader: ProtoReader, deadline: float) -> tuple[str, str, str, bytes] | tuple[str]:
    while True:
        line = reader.read_line(deadline)
        if line.startswith(b"INFO "):
            continue
        if line == b"PONG\r\n":
            return ("PONG",)
        if line.startswith(b"-ERR"):
            raise ProtocolError(line.decode("utf-8", "replace").rstrip())
        if not line.startswith(b"MSG "):
            raise ProtocolError(f"unexpected server line: {line!r}")

        parts = line.rstrip(b"\r\n").split(b" ")
        if len(parts) != 4:
            raise ProtocolError(f"bad MSG line: {line!r}")
        subject = parts[1].decode("ascii")
        sid = parts[2].decode("ascii")
        payload_len = int(parts[3])
        payload = reader.read_exact(payload_len, deadline)
        trailer = reader.read_exact(2, deadline)
        if trailer != b"\r\n":
            raise ProtocolError(f"bad MSG trailer: {trailer!r}")
        return ("MSG", subject, sid, payload)


def connect_client(host: str, port: int, deadline: float) -> tuple[socket.socket, ProtoReader]:
    sock = socket.create_connection((host, port), timeout=max(0.1, deadline - time.monotonic()))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    reader = ProtoReader(sock)
    line = reader.read_line(deadline)
    if not line.startswith(b"INFO "):
        raise ProtocolError(f"expected INFO, got {line!r}")
    sock.sendall(b"CONNECT {}\r\n")
    return sock, reader


def expected_metric(kind: str, subject_idx: int, sample_idx: int, base_step: int) -> float:
    base = subject_idx * base_step
    if kind == "count":
        return float(sample_idx + 1)
    window = 4 if kind == "avg4" else 8
    length = min(sample_idx + 1, window)
    first = sample_idx + 1 - length
    edge_sum = (first + sample_idx) * length / 2.0
    total = base * length + edge_sum
    return total / length if kind == "avg4" else float(total)


class Subscriber:
    def __init__(
        self,
        idx: int,
        host: str,
        port: int,
        subjects: int,
        messages_per_subject: int,
        base_step: int,
        timeout_s: float,
    ) -> None:
        self.idx = idx
        self.host = host
        self.port = port
        self.subjects = subjects
        self.messages_per_subject = messages_per_subject
        self.base_step = base_step
        self.timeout_s = timeout_s
        self.ready = threading.Event()
        self.done = threading.Event()
        self.error: BaseException | None = None
        self.sock: socket.socket | None = None
        self.raw_seen = 0
        self.frames_seen = 0
        self.metric_seen = {
            "avg4": [0] * subjects,
            "sum8": [0] * subjects,
            "count": [0] * subjects,
        }
        self.thread = threading.Thread(target=self.run, name=f"sub-{idx}", daemon=True)

    @property
    def expected_pubs(self) -> int:
        return self.subjects * self.messages_per_subject

    @property
    def expected_frames(self) -> int:
        return self.expected_pubs * 4

    def start(self) -> None:
        self.thread.start()

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()

    def run(self) -> None:
        deadline = time.monotonic() + self.timeout_s
        frames = 0
        try:
            sock, reader = connect_client(self.host, self.port, deadline)
            self.sock = sock
            sock.sendall(
                b"SUB load.* 1\r\n"
                b"SUB load.*.avg4 2\r\n"
                b"SUB load.*.sum8 3\r\n"
                b"SUB load.*.count 4\r\n"
                b"PING\r\n"
            )
            while True:
                op = read_server_op(reader, deadline)
                if op[0] == "PONG":
                    self.ready.set()
                    break

            deadline = time.monotonic() + self.timeout_s
            while frames < self.expected_frames:
                op = read_server_op(reader, deadline)
                if op[0] == "PONG":
                    continue
                _, subject, sid, payload = op
                self.check_msg(subject, sid, payload)
                frames += 1
                self.frames_seen = frames
            self.check_final_counts()
        except Exception as exc:
            self.error = exc
        finally:
            self.ready.set()
            self.done.set()
            self.close()

    def check_msg(self, subject: str, sid: str, payload: bytes) -> None:
        if sid == SID_RAW:
            self.check_raw(subject, payload)
            return
        if sid == SID_AVG4:
            self.check_metric("avg4", subject, payload)
            return
        if sid == SID_SUM8:
            self.check_metric("sum8", subject, payload)
            return
        if sid == SID_COUNT:
            self.check_metric("count", subject, payload)
            return
        raise ProtocolError(f"subscriber {self.idx}: unexpected sid {sid!r}")

    def check_raw(self, subject: str, payload: bytes) -> None:
        subject_idx = self.raw_seen % self.subjects
        local_idx = self.raw_seen // self.subjects
        want_subject = f"load.{subject_idx}"
        want_payload = str(subject_idx * self.base_step + local_idx).encode("ascii")
        if subject != want_subject or payload != want_payload:
            raise ProtocolError(
                f"subscriber {self.idx}: raw #{self.raw_seen} got {subject}|{payload!r}, "
                f"want {want_subject}|{want_payload!r}"
            )
        self.raw_seen += 1

    def parse_metric_subject(self, kind: str, subject: str) -> int:
        parts = subject.split(".")
        if len(parts) != 3 or parts[0] != "load" or parts[2] != kind:
            raise ProtocolError(f"subscriber {self.idx}: bad {kind} subject {subject!r}")
        subject_idx = int(parts[1])
        if not 0 <= subject_idx < self.subjects:
            raise ProtocolError(f"subscriber {self.idx}: subject index out of range in {subject!r}")
        return subject_idx

    def check_metric(self, kind: str, subject: str, payload: bytes) -> None:
        subject_idx = self.parse_metric_subject(kind, subject)
        sample_idx = self.metric_seen[kind][subject_idx]
        got = float(payload.decode("ascii"))
        want = expected_metric(kind, subject_idx, sample_idx, self.base_step)
        if not math.isclose(got, want, rel_tol=0.0, abs_tol=1e-9):
            raise ProtocolError(
                f"subscriber {self.idx}: {subject} sample {sample_idx} got {got}, want {want}"
            )
        self.metric_seen[kind][subject_idx] += 1

    def check_final_counts(self) -> None:
        if self.raw_seen != self.expected_pubs:
            raise ProtocolError(f"subscriber {self.idx}: raw count {self.raw_seen}, want {self.expected_pubs}")
        for kind, counts in self.metric_seen.items():
            for subject_idx, count in enumerate(counts):
                if count != self.messages_per_subject:
                    raise ProtocolError(
                        f"subscriber {self.idx}: {kind} load.{subject_idx} count "
                        f"{count}, want {self.messages_per_subject}"
                    )


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be > 0")
    return parsed


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be > 0")
    return parsed


def find_free_port(host: str) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def wait_for_port(host: str, port: int, proc: subprocess.Popen[object], timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    last_error: BaseException | None = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"monoblok exited early with status {proc.returncode}")
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {host}:{port}: {last_error}")


def publish_all(host: str, port: int, subjects: int, messages_per_subject: int, base_step: int, timeout_s: float) -> float:
    deadline = time.monotonic() + timeout_s
    sock, reader = connect_client(host, port, deadline)
    started = time.monotonic()
    try:
        pending = bytearray()
        for local_idx in range(messages_per_subject):
            for subject_idx in range(subjects):
                subject = f"load.{subject_idx}".encode("ascii")
                payload = str(subject_idx * base_step + local_idx).encode("ascii")
                pending.extend(b"PUB ")
                pending.extend(subject)
                pending.extend(b" ")
                pending.extend(str(len(payload)).encode("ascii"))
                pending.extend(b"\r\n")
                pending.extend(payload)
                pending.extend(b"\r\n")
                if len(pending) >= 65536:
                    sock.sendall(pending)
                    pending.clear()
        if pending:
            sock.sendall(pending)
        sock.sendall(b"PING\r\n")
        while True:
            op = read_server_op(reader, deadline)
            if op[0] == "PONG":
                return time.monotonic() - started
    finally:
        sock.close()


def tail(path: Path, max_bytes: int = 12000) -> str:
    if not path.exists():
        return ""
    data = path.read_bytes()
    return data[-max_bytes:].decode("utf-8", "replace")


def progress_summary(subscribers: list[Subscriber]) -> str:
    if not subscribers:
        return "subscribers=0"
    ready = sum(1 for sub in subscribers if sub.ready.is_set())
    done = sum(1 for sub in subscribers if sub.done.is_set())
    errors = sum(1 for sub in subscribers if sub.error is not None)
    frames = [sub.frames_seen for sub in subscribers]
    return (
        f"subscribers={len(subscribers)} ready={ready} done={done} errors={errors} "
        f"frames=min:{min(frames)} max:{max(frames)}"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("monoblok", help="path to monoblok binary")
    parser.add_argument("--host", default=os.environ.get("MB_LOAD_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("MB_LOAD_PORT", "0")))
    parser.add_argument("--subscribers", type=positive_int, default=int(os.environ.get("MB_LOAD_SUBSCRIBERS", "32")))
    parser.add_argument("--subjects", type=positive_int, default=int(os.environ.get("MB_LOAD_SUBJECTS", "3")))
    parser.add_argument(
        "--messages-per-subject",
        type=positive_int,
        default=int(os.environ.get("MB_LOAD_MESSAGES_PER_SUBJECT", "512")),
    )
    parser.add_argument("--timeout", type=positive_float, default=float(os.environ.get("MB_LOAD_TIMEOUT", "30")))
    parser.add_argument("--keep-temp", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    host = args.host
    port = args.port if args.port != 0 else find_free_port(host)
    total_pubs = args.subjects * args.messages_per_subject
    base_step = max(args.messages_per_subject + 1, 1000000)

    tmp = Path(tempfile.mkdtemp(prefix="monoblok-load-smoke-"))
    patchbay = tmp / "patchbay.edn"
    stdout_path = tmp / "monoblok.out"
    stderr_path = tmp / "monoblok.err"
    patchbay.write_text(PATCHBAY, encoding="utf-8")

    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        proc = subprocess.Popen(
            [args.monoblok, "--host", host, "--port", str(port), "--patchbay", str(patchbay)],
            stdout=stdout,
            stderr=stderr,
        )
        subscribers: list[Subscriber] = []
        started = time.monotonic()
        phase = "starting"
        try:
            phase = "waiting for port"
            wait_for_port(host, port, proc, args.timeout)
            phase = "starting subscribers"
            subscribers = [
                Subscriber(i, host, port, args.subjects, args.messages_per_subject, base_step, args.timeout)
                for i in range(args.subscribers)
            ]
            for sub in subscribers:
                sub.start()
            for sub in subscribers:
                phase = f"waiting for subscriber {sub.idx} readiness"
                if not sub.ready.wait(args.timeout):
                    raise TimeoutError(f"subscriber {sub.idx} did not become ready")
                if sub.error is not None:
                    raise sub.error

            phase = "publishing"
            elapsed = publish_all(host, port, args.subjects, args.messages_per_subject, base_step, args.timeout)

            deadline = time.monotonic() + args.timeout
            for sub in subscribers:
                phase = f"waiting for subscriber {sub.idx} drain"
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not sub.done.wait(remaining):
                    raise TimeoutError(f"subscriber {sub.idx} did not receive all frames")
                if sub.error is not None:
                    raise sub.error

            frames_per_sub = total_pubs * 4
            delivered = frames_per_sub * args.subscribers
            print(
                "load smoke passed: "
                f"subscribers={args.subscribers} subjects={args.subjects} "
                f"pubs={total_pubs} frames/sub={frames_per_sub} "
                f"delivered={delivered} publish_elapsed={elapsed:.3f}s"
            )
            return 0
        except KeyboardInterrupt:
            print(
                f"load smoke interrupted: phase={phase} elapsed={time.monotonic() - started:.3f}s "
                f"{progress_summary(subscribers)}",
                file=sys.stderr,
            )
            raise
        except Exception as exc:
            print(f"load smoke failed: {exc}", file=sys.stderr)
            print(
                f"load smoke phase: {phase} elapsed={time.monotonic() - started:.3f}s "
                f"{progress_summary(subscribers)}",
                file=sys.stderr,
            )
            print("--- monoblok stderr ---", file=sys.stderr)
            print(tail(stderr_path), file=sys.stderr)
            print("--- monoblok stdout ---", file=sys.stderr)
            print(tail(stdout_path), file=sys.stderr)
            return 1
        finally:
            for sub in subscribers:
                sub.close()
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            if args.keep_temp:
                print(f"kept temp files in {tmp}")
            else:
                shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
