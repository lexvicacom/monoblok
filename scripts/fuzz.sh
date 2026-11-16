#!/bin/sh
# Run every libFuzzer harness for a fixed time budget.
#
#   scripts/fuzz.sh [SECONDS_PER_TARGET] [TARGET...]
#
# Builds build-fuzz/ with clang if needed. Seed corpora in fuzz/corpus/<name>
# are read-only inputs; new interesting inputs go to build-fuzz/corpus/<name>.
# Crash reproducers land in build-fuzz/artifacts/. Exit status is non-zero if
# any harness found a crash, leak, or timeout.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="${MB_FUZZ_BUILD_DIR:-$ROOT/build-fuzz}"
SECS="${1:-60}"
[ "$#" -gt 0 ] && shift
TARGETS="$*"
[ -n "$TARGETS" ] || TARGETS="proto http patchbay_edn patchbay_json patchbay_yaml regex"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    CC="${CC:-clang}" cmake -S "$ROOT" -B "$BUILD" -DMB_FUZZ=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi
cmake --build "$BUILD" --target fuzzers --parallel

mkdir -p "$BUILD/artifacts"
status=0
for name in $TARGETS; do
    bin="$BUILD/fuzz/fuzz-$name"
    seeds="$ROOT/fuzz/corpus/$name"
    work="$BUILD/corpus/$name"
    mkdir -p "$work"
    dict_arg=""
    [ -f "$ROOT/fuzz/dict/$name.dict" ] && dict_arg="-dict=$ROOT/fuzz/dict/$name.dict"
    echo "== fuzz-$name (${SECS}s)"
    # shellcheck disable=SC2086
    if ! "$bin" $dict_arg -max_total_time="$SECS" -timeout=10 -rss_limit_mb=2048 \
            -print_final_stats=1 -artifact_prefix="$BUILD/artifacts/$name-" \
            "$work" "$seeds"; then
        echo "!! fuzz-$name failed; reproducers in $BUILD/artifacts/"
        status=1
    fi
done
exit "$status"
