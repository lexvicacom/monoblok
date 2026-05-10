#!/usr/bin/env python3
"""Publish randomized Moonwell lunar rig telemetry into a NATS server."""

from __future__ import annotations

import argparse
import json
import random
import socket
import time
from dataclasses import dataclass


RIGS = [f"rig-{n:02d}" for n in range(1, 10)]
SEAL_STATES = ["LOCKED", "LOCKED", "LOCKED", "LOCKED", "CYCLE", "UNSEALED"]
DRILL_MODES = ["AUTO", "AUTO", "AUTO", "AUTO", "MANUAL", "SAFE", "FAULT"]
BUS_STATES = ["NOMINAL", "NOMINAL", "NOMINAL", "NOMINAL", "BROWNOUT", "ISLAND"]
LOG_LINES = [
    "INFO regolith conveyor nominal",
    "INFO cold trap scan complete",
    "WARN heater duty cycle high",
    "WARN dust curtain saturated",
    "ERROR auger torque limiter tripped",
    "ICE_VOID detected under drill string",
]


@dataclass
class RigState:
    temp: float
    pressure: float
    dust: float
    radiation: float
    ice: float
    depth: float
    torque: float
    rpm: float
    vibration: float
    bit_temp: float
    battery: float
    solar: float
    load: float
    heater: float

    @classmethod
    def fresh(cls) -> "RigState":
        return cls(
            temp=random.uniform(-158.0, -128.0),
            pressure=random.uniform(66.0, 74.0),
            dust=random.uniform(0.04, 0.24),
            radiation=random.uniform(0.04, 0.13),
            ice=random.uniform(22.0, 58.0),
            depth=random.uniform(0.0, 12.0),
            torque=random.uniform(38.0, 72.0),
            rpm=random.uniform(70.0, 125.0),
            vibration=random.uniform(0.12, 0.44),
            bit_temp=random.uniform(42.0, 95.0),
            battery=random.uniform(48.0, 96.0),
            solar=random.uniform(180.0, 620.0),
            load=random.uniform(210.0, 520.0),
            heater=random.uniform(20.0, 68.0),
        )

    def nudge(self) -> None:
        self.temp = clamp(-190.0, -90.0, self.temp + random.gauss(0, 1.1))
        self.pressure = clamp(48.0, 82.0, self.pressure + random.gauss(0, 0.55))
        self.dust = clamp(0.0, 1.1, self.dust + random.gauss(0, 0.025))
        self.radiation = clamp(0.0, 0.42, self.radiation + random.gauss(0, 0.007))
        self.ice = clamp(0.0, 88.0, self.ice + random.gauss(0, 1.4))
        self.depth = clamp(0.0, 80.0, self.depth + random.uniform(0.02, 0.22))
        self.torque = clamp(10.0, 135.0, self.torque + random.gauss(0, 4.5))
        self.rpm = clamp(0.0, 170.0, self.rpm + random.gauss(0, 7.0))
        self.vibration = clamp(0.0, 1.5, self.vibration + random.gauss(0, 0.035))
        self.bit_temp = clamp(-80.0, 165.0, self.bit_temp + random.gauss(0, 3.0))
        self.battery = clamp(3.0, 100.0, self.battery - random.uniform(0.0, 0.09))
        self.solar = clamp(0.0, 820.0, self.solar + random.gauss(0, 34.0))
        self.load = clamp(90.0, 900.0, self.load + random.gauss(0, 30.0))
        self.heater = clamp(0.0, 100.0, self.heater + random.gauss(0, 5.0))

        if random.random() < 0.018:
            self.pressure -= random.uniform(6.0, 12.0)
        if random.random() < 0.018:
            self.dust += random.uniform(0.35, 0.7)
        if random.random() < 0.012:
            self.radiation += random.uniform(0.09, 0.2)
        if random.random() < 0.025:
            self.torque += random.uniform(28.0, 55.0)
        if random.random() < 0.022:
            self.vibration += random.uniform(0.45, 0.85)
        if random.random() < 0.018:
            self.solar *= random.uniform(0.05, 0.35)

    def env_payload(self) -> dict[str, object]:
        return {
            "sensor": {
                "thermal": {"temp": round(self.temp, 1)},
                "atmo": {
                    "pressure": round(self.pressure, 1),
                    "dust": round(self.dust, 3),
                    "radiation": round(self.radiation, 3),
                },
                "prospect": {"ice": round(self.ice, 1)},
                "hatch": {"seal": random.choice(SEAL_STATES)},
            },
        }

    def drill_payload(self) -> dict[str, object]:
        return {
            "sensor": {
                "bore": {"depth": round(self.depth, 2)},
                "motor": {
                    "torque": round(self.torque, 1),
                    "rpm": round(self.rpm, 1),
                    "mode": random.choice(DRILL_MODES),
                },
                "bit": {
                    "vibration": round(self.vibration, 3),
                    "temp": round(self.bit_temp, 1),
                },
            },
        }

    def power_payload(self) -> dict[str, object]:
        return {
            "sensor": {
                "storage": {"battery": round(self.battery, 1)},
                "array": {"solar": round(self.solar, 1)},
                "bus": {
                    "load": round(self.load, 1),
                    "heater": round(self.heater, 1),
                    "state": random.choice(BUS_STATES),
                },
            },
        }


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


def clamp(low: float, high: float, value: float) -> float:
    return max(low, min(high, value))


def publish_json(pub: NatsPublisher, subject: str, value: dict[str, object]) -> None:
    pub.publish(subject, json.dumps(value, separators=(",", ":")))


def run(args: argparse.Namespace) -> None:
    random.seed(args.seed)
    states = {rig: RigState.fresh() for rig in RIGS[: args.rigs]}
    pub = NatsPublisher(args.host, args.port, "moonwell-random-pump")
    sent = 0

    try:
        while args.count == 0 or sent < args.count:
            rig = random.choice(list(states))
            state = states[rig]
            state.nudge()

            publish_json(pub, f"moonwell.{rig}.env", state.env_payload())
            sent += 1

            if random.random() < args.drill_chance:
                publish_json(pub, f"moonwell.{rig}.drill", state.drill_payload())
                sent += 1

            if random.random() < args.power_chance:
                publish_json(pub, f"moonwell.{rig}.power", state.power_payload())
                sent += 1

            if random.random() < args.log_chance:
                pub.publish(f"moonwell.{rig}.log", random.choice(LOG_LINES))
                sent += 1

            if args.verbose:
                print(f"published batch for {rig} ({sent} messages total)")

            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        pub.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pump randomized Moonwell rig JSON telemetry into NATS."
    )
    parser.add_argument("--host", default="127.0.0.1", help="NATS host")
    parser.add_argument("--port", type=int, default=4222, help="NATS port")
    parser.add_argument("--rigs", type=int, default=6, choices=range(1, len(RIGS) + 1))
    parser.add_argument(
        "--interval",
        type=float,
        default=0.25,
        help="seconds to sleep between env publishes",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=0,
        help="messages to publish before exiting; 0 runs until interrupted",
    )
    parser.add_argument("--seed", type=int, default=None, help="random seed")
    parser.add_argument(
        "--drill-chance",
        type=float,
        default=0.7,
        help="chance that each env frame is followed by drill JSON",
    )
    parser.add_argument(
        "--power-chance",
        type=float,
        default=0.55,
        help="chance that each env frame is followed by power JSON",
    )
    parser.add_argument(
        "--log-chance",
        type=float,
        default=0.12,
        help="chance that each env frame is followed by a log publish",
    )
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
