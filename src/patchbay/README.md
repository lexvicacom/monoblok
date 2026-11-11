# Patchbay Internals

Patchbay code is intentionally shallow and direct. The main pieces are:

- `pb_sexpr.c`: EDN-like parser for hand-written patchbay files.
- `pb_json.c`: JSON patchbay adapter for tooling compatibility.
- `pb_yaml.c`: small YAML-sugar parser/lowerer for `.yml`/`.yaml` patchbays.
- `pb_eval.c`: evaluator core, special forms, and the `FORMS[]` syntax registry.
- `pb_forms.c`: eager form dispatch plus included implementation fragments.
- `pb_form_*.c`: form implementation fragments. They are included by `pb_forms.c`
  and marked `HEADER_FILE_ONLY` in CMake so editors can open them as C without
  compiling them as separate translation units.
- `pb_program.c`: top-level program loading, rule dispatch, ticking, and publish
  integration.

## Adding A Form

`FORMS[]` in `pb_eval.c` is the single description of a form. Adding one is an
enum id, one table row, and one dispatch case:

1. Add a `PB_FORM_*` id to `pb_eval_internal.h`.
2. Add a row to `FORMS[]` in `pb_eval.c`: spelling, id, and flags.
   - `.special`: the form evaluates its own raw arguments (see below).
   - `.stateful`: the form calls `state_slot()`. Slots are keyed by
     `(rule, call site, op, subject)`, so two calls of one form in a rule never
     share state. Snapshot restore uses the table to check an op still belongs
     to the form at its site (`pb_form_owns_op`), so the op name passed to
     `state_slot()` must be the spelling without a trailing `!`, optionally
     followed by `/t` or `/m`.
   - `.clocked`: the form always needs the clock timer.
   - `.clocked_ms`: it needs the timer only when called with a leading `:ms`
     window.
   Program loading derives `uses_clock_timer` from these flags; do not add name
   lists to `pb_program.c`.
3. Put the implementation in the closest `pb_form_*.c` fragment and list the
   spelling in that fragment's `Forms:` comment.
4. Add the `case` in `pb_eval_call_form()` in `pb_forms.c` (special forms get a
   case in `dispatch_call()` in `pb_eval.c` instead).
5. Add or extend coverage in `test/eval_unit.c`; use program/smoke tests if the
   form affects publish behavior, ticking, snapshots, or re-entry.

Special forms that need raw unevaluated arguments belong in `pb_eval.c`, not
`pb_forms.c`. Normal forms receive already-evaluated arguments.

## Resolve Pass

`pb_resolve_rule()` runs once per rule body on the AST (so EDN, JSON, and YAML
share it), at program load, `--validate`, and `--soundcheck`. It:

- numbers each call (list node) with a per-rule call-site id in `pb_value.tag`
  and tags each head symbol with its `FORMS[]` index + 1, so publish-time
  dispatch does no string compares;
- tags bound symbols (`subject`, `payload`, `payload-float`, `payload-int`,
  `payload-bool`, `replaying?`, `reply-to`);
- rejects unknown forms and symbols in every branch unless a `user_call` or
  `user_symbol` hook is installed;
- rewrites a bare symbol `->` step such as `(-> x squelch)` into a one-element
  call so it gets its own site, and rejects `on-silence` and `dropout` as
  `->` steps (their bodies must live in the parse arena, not
  scratch, and a threaded value would become a trailing body form).

State slots are keyed by `(rule, site, op, subject)`. The entry keeps the
subject that created it, so clock ticks bind `subject` to it. To roll several
subjects into one slot, funnel them onto one subject with a `:reentrant true`
rule and aggregate in a second rule. Snapshot format v5 persists the site id and
subject.

`(last SUBJECT)` is not state: it calls the `ctx->last` hook, which the program
points at the router LVC and soundcheck points at a small table. The evaluator
never includes router headers.

`eval_call()` sets `ctx->site` while a form runs and restores it afterwards;
`state_slot()` reads it. Evaluating an unresolved AST still works (tests do),
but every call then has site 0 and shares state like the old behavior.

`(env "NAME")` is deliberately not an evaluator form. It is a load-time string
source consumed by top-level config readers in `pb_program.c`; YAML `env: NAME`
lowers to the same AST shape.

## Every Rules And Reply

`(every :ms N [:reentrant BOOL] BODY...)` is a top-level sibling of `on`, loaded
by `pb_every_parse()` (shared by the loader, `--validate`, and `--soundcheck`).
It becomes a `pb_rule` with an empty filter and `every_ms` set; the rule index
skips it and `eval_rule_for_publish()` refuses it, so no publish ever evaluates
it. One `PB_EVAL_CLOCK_EVERY` slot is created at load (`pb_eval_every_arm()`)
with deadline 0, meaning "not yet scheduled": the first tick sets now + N without
firing. Later ticks fire once and advance by N, or realign to now + N when more
than one interval was missed (no burst). `tick_every()` runs the body with empty
`subject`/`payload` and nil `reply-to`, so slots created inside have the empty
subject as identity and bind it at tick time. EVERY slots are not snapshotted; the rule keeps its index and the
empty filter in snapshot identity. `(every ...)` inside a rule body is a resolve
error.

`reply-to` is a bound symbol backed by `ctx->reply_to` (empty means nil), and
`(reply! VALUE)` prepends it to a `publish!` argument list, so subject
validation is shared. It is a counted no-op with no reply subject or a nil value.

## Result Helpers

The short names below (`ok`, `nil`, `fail`, ...) are macros defined in
`pb_form_chunk.h` and visible only to `pb_eval.c` and the form fragments; other
files use the `pb_eval_*` names.

Use `ok(value)` for successful values, `nil()` for successful nil results, and
`fail(error)` for evaluator errors. `nil()` is not an error: it is how many forms
represent a filtered value, missing lookup, no-op effect, or ordinary completion
of a side-effecting form.

## Cost Discipline

Patchbay forms run on Monoblok's single libuv loop during publish handling. Be
careful with functionality that can drag the loop: unbounded scans, blocking I/O,
large heap allocation, expensive parsing, or hidden background work do not belong
in ordinary per-message forms. Prefer bounded work, arena scratch allocation, and
state initialized at config load or first state-slot creation.
