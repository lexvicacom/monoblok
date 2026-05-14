#!/usr/bin/env bash
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/monoblok"

if [ ! -x "$BIN" ]; then
    cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$ROOT/build" --target monoblok >/dev/null
fi

exec "$ROOT/test/smoke.sh" "$BIN"
