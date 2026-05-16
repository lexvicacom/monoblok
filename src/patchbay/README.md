# Patchbay Internals

Patchbay code is intentionally shallow and direct. The main pieces are:

- `pb_sexpr.c`: EDN-like parser for hand-written patchbay files.
- `pb_json.c`: JSON patchbay adapter for tooling compatibility.
- `pb_eval.c`: evaluator core, special forms, and the `FORMS[]` syntax registry.
- `pb_forms.c`: eager form dispatch plus included implementation fragments.
- `pb_form_*.c`: form implementation fragments. They are included by `pb_forms.c`
  and marked `HEADER_FILE_ONLY` in CMake so editors can open them as C without
  compiling them as separate translation units.
- `pb_program.c`: top-level program loading, rule dispatch, ticking, and publish
  integration.

## Adding A Form

1. Add a `PB_FORM_*` id to `pb_eval_internal.h`.
2. Add the public spelling to `FORMS[]` in `pb_eval.c`.
3. Put the implementation in the closest `pb_form_*.c` fragment and list the
   spelling in that fragment's `Forms:` comment.
4. Wire the id in `pb_eval_call_form()` in `pb_forms.c`.
5. Add or extend coverage in `test/eval_unit.c`; use program/smoke tests if the
   form affects publish behavior, ticking, snapshots, or re-entry.

Special forms that need raw unevaluated arguments belong in `pb_eval.c`, not
`pb_forms.c`. Normal forms receive already-evaluated arguments.

## Result Helpers

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
