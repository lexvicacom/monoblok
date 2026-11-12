# Tinyblok and Patchbay Lite

## Target runtime: monoblok vs Tinyblok

This skill covers both monoblok and Tinyblok/Patchbay Lite. Before authoring
target-specific rules, determine the target runtime:

- If the repo, path, command, or prompt clearly says monoblok daemon, use full
  monoblok patchbay.
- If it clearly says Tinyblok, ESP32, firmware, microcontroller, or Patchbay
  Lite, use Patchbay Lite.
- If the target is ambiguous and the answer would differ, ask the user to
  confirm: monoblok daemon or Tinyblok/Patchbay Lite. If the requested rule is
  genuinely portable ordinary `(on ...)` patchbay with no target-specific
  features, you may proceed and keep it in the portable subset.

For portable rules, prefer ordinary `(on ...)` rules; scalar bound values like
`subject`, `payload`, `payload-float`, and `payload-int`; arithmetic,
comparison, string, subject, state, window, clock, edge, bar, and `publish!`
forms. This subset works for both runtimes unless a local build disables a form.

Alert the user before using or recommending full monoblok-only features for a
Tinyblok file. This includes top-level `(every ...)`, `(lvc ...)` and `(export ...)`,
`hysteresis`, `ewma`, `last`, the funnel pattern, monoblok's `reply!` (see
[Monoblok request-reply](monoblok.md#request-reply-monoblok); Tinyblok's `reply!` is different),
`(import ...)` and JetStream ingress, `replaying?`, daemon flags such as
`--snapshot`, runtime patch/config
loading, dynamic graph edits, server-side subscriptions beyond Tinyblok's fixed
request subjects, inbound/outbound bridges, LVC/snapshot policy, and
fleet-management behavior. Tinyblok's vendored core can parse `lvc` and
`export`/deprecated `bridge`, but they are not wired into firmware policy.

JSON/event-document processing is also outside Patchbay Lite by default.
Tinyblok can compile `json-get` and `json-demux!` behind
`CONFIG_TINYBLOK_PATCHBAY_JSON`, but that option is disabled by default for
firmware size. Do not introduce JSON forms in a Tinyblok patchbay unless the
user explicitly enables that Kconfig option and accepts the size tradeoff.

## Patchbay Lite additions

Tinyblok embeds `patchbay.edn` into the firmware image and parses it once at
boot. It supports normal `(on ...)` rules plus these Tinyblok-specific forms:

- `(pump "subject" :from c_symbol :type TYPE :hz N)` registers a timed local
  source. `TYPE` is `u32`, `i32`, `f32`, `u64`, or `uptime-s`; the C source is a
  zero-argument read such as `uint32_t name(void)`, `int name(void)`,
  `float name(void)`, or `uint64_t name(void)`. Add user-owned implementations
  to `main/c/user.c` or another C file listed by `main/CMakeLists.txt`, then
  register the native symbol in `main/c/tinyblok_patchbay.c`.
- `(fn name :from c_symbol :type TYPE)` exposes a compiled zero-argument helper
  as a rule value. `(fn name :from c_symbol :input bytes :type bytes)` exposes a
  byte transformer that can be called from request handlers as `(name payload)`.
  Scalar `:input` functions are firmware-version dependent; verify the local
  Tinyblok native symbol table before using them.
- `(on-req "subject" BODY)` registers a fixed NATS request subject. Tinyblok
  subscribes to it after each broker connect, evaluates `BODY`, and sends
  replies to the requester's reply inbox.
- `(reply! VALUE)` sends a request reply from inside `on-req` (Tinyblok only;
  full monoblok uses `reply!` inside ordinary `(on ...)` rules, see
  [Monoblok request-reply](monoblok.md#request-reply-monoblok)). In a pipeline,
  `(-> uptime-s (reply!))` replies with the threaded value. With no argument,
  `reply!` replies with the original request payload.

Tinyblok's `publish!` queues outbound records into the firmware TX ring for the
main loop to drain to NATS. Keep pump, request, and publish paths deterministic:
no per-tick heap allocation, blocking I/O, runtime parsing, or unbounded work.
