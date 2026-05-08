#!/usr/bin/env python3
"""Publish randomized Cloudhook kite-turbine telemetry into a NATS server."""

from __future__ import annotations

import argparse
import json
import random
import socket
import time
from dataclasses import dataclass


KITES = [f"kite-{n:02d}" for n in range(1, 10)]
MODES = ["LOOP", "LOOP", "LOOP", "CLIMB", "HOLD", "LAND", "FAULT"]
INVERTERS = ["GRID", "GRID", "GRID", "GRID", "STARTUP", "ISLAND", "FAULT"]
LOG_LINES = [
    "INFO crosswind loop stable",
    "INFO winch trim complete",
    "WARN tether fairlead temperature rising",
    "WARN salt spray on anemometer",
    "ERROR inverter sync lost",
    "LINE_SNAP predictor exceeded threshold",
    "BIRD_STRIKE acoustic signature rejected",
]


@dataclass
class KiteState:
    alt: float
    airspeed: float
    tether: float
    yaw: float
    roll: float
    wind: float
    gust: float
    shear: float
    pressure: float
    rain: float
    kw: float
    voltage: float
    battery: float
    brake: int

    @classmethod
    def fresh(cls) -> "KiteState":
        wind = random.uniform(18.0, 34.0)
        return cls(
            alt=random.uniform(480.0, 760.0),
            airspeed=wind + random.uniform(3.0, 11.0),
            tether=random.uniform(28.0, 54.0),
            yaw=random.uniform(-8.0, 8.0),
            roll=random.uniform(-16.0, 16.0),
            wind=wind,
            gust=wind + random.uniform(3.0, 11.0),
            shear=random.uniform(0.04, 0.2),
            pressure=random.uniform(996.0, 1012.0),
            rain=random.uniform(0.0, 1.2),
            kw=random.uniform(42.0, 118.0),
            voltage=random.uniform(675.0, 742.0),
            battery=random.uniform(45.0, 94.0),
            brake=random.choice([0, 0, 0, 0, 1]),
        )

    def nudge(self) -> None:
        self.wind = clamp(2.0, 58.0, self.wind + random.gauss(0, 1.0))
        self.gust = clamp(self.wind, 75.0, self.wind + abs(random.gauss(6.0, 4.0)))
        self.alt = clamp(180.0, 1100.0, self.alt + random.gauss(0, 18.0))
        self.airspeed = clamp(4.0, 85.0, self.wind + random.gauss(7.5, 2.8))
        self.tether = clamp(4.0, 98.0, self.tether + random.gauss(0, 2.4))
        self.yaw = clamp(-45.0, 45.0, self.yaw + random.gauss(0, 2.5))
        self.roll = clamp(-55.0, 55.0, self.roll + random.gauss(0, 3.8))
        self.shear = clamp(0.0, 0.65, self.shear + random.gauss(0, 0.018))
        self.pressure = clamp(970.0, 1035.0, self.pressure + random.gauss(0, 0.55))
        self.rain = clamp(0.0, 18.0, self.rain + random.gauss(0, 0.35))
        self.kw = clamp(0.0, 190.0, self.wind * 3.1 + random.gauss(0, 10.0))
        self.voltage = clamp(580.0, 835.0, self.voltage + random.gauss(0, 9.0))
        self.battery = clamp(2.0, 100.0, self.battery + (self.kw - 80.0) * 0.001)
        self.brake = random.choice([0, 0, 0, 0, 0, 1])

        if random.random() < 0.02:
            self.gust += random.uniform(18.0, 30.0)
        if random.random() < 0.018:
            self.shear += random.uniform(0.18, 0.35)
        if random.random() < 0.018:
            self.tether += random.uniform(18.0, 34.0)
        if random.random() < 0.016:
            self.alt += random.choice([-190.0, 210.0])
        if random.random() < 0.014:
            self.voltage += random.choice([-90.0, 95.0])
        if random.random() < 0.018:
            self.rain += random.uniform(5.0, 11.0)

    def flight_payload(self) -> dict[str, object]:
        return {
            "alt": round(self.alt, 1),
            "airspeed": round(self.airspeed, 1),
            "tether": round(self.tether, 1),
            "yaw": round(self.yaw, 1),
            "roll": round(self.roll, 1),
            "mode": random.choice(MODES),
        }

    def weather_payload(self) -> dict[str, object]:
        return {
            "wind": round(self.wind, 1),
            "gust": round(self.gust, 1),
            "shear": round(self.shear, 3),
            "pressure": round(self.pressure, 1),
            "rain": round(self.rain, 1),
        }

    def power_payload(self) -> dict[str, object]:
        return {
            "kw": round(self.kw, 1),
            "voltage": round(self.voltage, 1),
            "battery": round(self.battery, 1),
            "inverter": random.choice(INVERTERS),
            "brake": self.brake,
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
    states = {kite: KiteState.fresh() for kite in KITES[: args.kites]}
    pub = NatsPublisher(args.host, args.port, "cloudhook-random-pump")
    sent = 0

    try:
        while args.count == 0 or sent < args.count:
            kite = random.choice(list(states))
            state = states[kite]
            state.nudge()

            publish_json(pub, f"cloudhook.{kite}.flight", state.flight_payload())
            sent += 1

            if random.random() < args.weather_chance:
                publish_json(pub, f"cloudhook.{kite}.weather", state.weather_payload())
                sent += 1

            if random.random() < args.power_chance:
                publish_json(pub, f"cloudhook.{kite}.power", state.power_payload())
                sent += 1

            if random.random() < args.log_chance:
                pub.publish(f"cloudhook.{kite}.log", random.choice(LOG_LINES))
                sent += 1

            if args.verbose:
                print(f"published batch for {kite} ({sent} messages total)")

            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        pub.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pump randomized Cloudhook kite-turbine JSON telemetry into NATS."
    )
    parser.add_argument("--host", default="127.0.0.1", help="NATS host")
    parser.add_argument("--port", type=int, default=4222, help="NATS port")
    parser.add_argument(
        "--kites", type=int, default=6, choices=range(1, len(KITES) + 1)
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=0.22,
        help="seconds to sleep between flight publishes",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=0,
        help="messages to publish before exiting; 0 runs until interrupted",
    )
    parser.add_argument("--seed", type=int, default=None, help="random seed")
    parser.add_argument(
        "--weather-chance",
        type=float,
        default=0.75,
        help="chance that each flight frame is followed by weather JSON",
    )
    parser.add_argument(
        "--power-chance",
        type=float,
        default=0.65,
        help="chance that each flight frame is followed by power JSON",
    )
    parser.add_argument(
        "--log-chance",
        type=float,
        default=0.1,
        help="chance that each flight frame is followed by a log publish",
    )
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
