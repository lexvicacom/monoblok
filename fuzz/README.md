# Fuzzing

libFuzzer harnesses for the parsers that see untrusted or semi-trusted bytes.
Every harness runs under AddressSanitizer and UBSan, so out-of-bounds reads,
overflow, undefined behaviour and leaks fail the run, not only crashes.

| Harness         | Code under test                                   | Input source            |
|-----------------|---------------------------------------------------|-------------------------|
| `proto`         | `src/proto.c` NATS client protocol parser         | client sockets          |
| `http`          | `src/server/http_parse.c` HTTP/SSE request parser | HTTP adapter sockets    |
| `patchbay_edn`  | `pb_sexpr.c` EDN reader + program load/validate   | rule files              |
| `patchbay_json` | `pb_json.c` JSON rule adapter                     | rule files / tooling    |
| `patchbay_yaml` | `pb_yaml.c` YAML sugar lowerer                    | rule files              |
| `regex`         | `pb_regex.c` / `pb_regex_dfa.c` compile + match   | rule patterns, payloads |

## Running

Requires clang with the compiler-rt sanitizer runtime (on Debian/Ubuntu,
`libclang-rt-<ver>-dev`; Apple clang does not ship libFuzzer, so use Homebrew
`llvm` on macOS).

```sh
scripts/fuzz.sh            # every harness, 60s each
scripts/fuzz.sh 600 proto  # one harness, 10 minutes
```

Or by hand:

```sh
CC=clang cmake -S . -B build-fuzz -DMB_FUZZ=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz --target fuzzers
build-fuzz/fuzz/fuzz-proto -dict=fuzz/dict/proto.dict /tmp/proto-work fuzz/corpus/proto
```

`fuzz/corpus/<name>/` holds small, reviewed seeds and is passed read-only.
New interesting inputs go to the first directory argument (`build-fuzz/corpus/`
when using the script). Crash reproducers land in `build-fuzz/artifacts/`.

CI runs every harness for 60 seconds on each push and pull request and uploads
reproducers when a run fails.

## When a harness finds something

1. Reproduce: `build-fuzz/fuzz/fuzz-<name> build-fuzz/artifacts/<file>`.
2. Minimize: `build-fuzz/fuzz/fuzz-<name> -minimize_crash=1 -runs=100000 <file>`.
3. Fix the parser, then add the minimized input to `fuzz/corpus/<name>/` as
   `regression-<short-description>` so every later run replays it.

## Adding a harness

Add `fuzz/fuzz_<name>.c` defining `LLVMFuzzerTestOneInput`, register it with
`mb_add_fuzzer(<name>)` in `fuzz/CMakeLists.txt`, seed `fuzz/corpus/<name>/`,
and add the name to the default list in `scripts/fuzz.sh`. Harnesses should
be deterministic, free everything they allocate, and avoid files, sockets and
timers. Check API promises (slices inside the input, consumed length within
bounds, forward progress) with `__builtin_trap()` so a broken invariant shows
up as a crash.
