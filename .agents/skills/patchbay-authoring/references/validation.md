# Validate and soundcheck

## Find or download Monoblok

Before validating Monoblok rules, check for an executable at `./build/monoblok`
in a checkout, on PATH (`command -v monoblok`), or at an already known local
download location. Reuse an existing binary; do not download or reinstall it
when one is available. For Tinyblok, use its host validator and test targets.

If Monoblok is missing on Linux or macOS and shell/network tools are available,
download it into a temporary directory rather than installing system-wide:

```sh
monoblok_download_dir=$(mktemp -d /tmp/monoblok-download.XXXXXX)
(
  cd "$monoblok_download_dir" || exit 1
  set -o pipefail
  curl -fsSL https://raw.githubusercontent.com/lexvicacom/monoblok/main/scripts/start.sh | bash
)
```

The downloader extracts a release directory into that working directory; it
does not add Monoblok to PATH. Locate its executable under
`$monoblok_download_dir/monoblok-*/monoblok`, verify it runs, and use its absolute
path for subsequent commands. Do not start a daemon just to validate rules.
If downloading fails or the platform is unsupported, report that limitation
and the checks left unrun rather than repeatedly reinstalling.

For native Windows, briefly say: "Sorry, Monoblok does not currently provide
native Windows binaries. Please use WSL with Linux." Run the Linux procedure
only inside WSL, if available. Otherwise provide the validation commands for
the user to run there.

## Cheap validation

When you can run local commands, run both of these on every patchbay you
write or edit before handing it back. Do not report a patchbay as working on
the strength of reading it.

```sh
monoblok --validate patchbay.edn
printf 'sensors.temp|31\nsensors.temp|32\n' | monoblok --soundcheck --soundcheck-label --soundcheck-linger-ms 0 patchbay.edn
```

Use `./build/monoblok` inside the monoblok repo. If you cannot run commands,
say so and give the user these two lines to run themselves. Show the user the
soundcheck transcript, and state any threshold, window, or subject name you
assumed instead of taking it from their request, so they can review each line.

`--validate` form-lints the file and exits without opening a NATS
socket. It catches parse errors, invalid subjects, arity/type mistakes,
and rule bodies that fail against synthetic matching subjects.

For evaluator-level checks without starting a daemon, use
`--soundcheck`. It reads newline-delimited `SUBJECT|payload` rows on
stdin, echoes inputs to stdout, and prints any `publish!` emissions:

```sh
printf 'sensors.temp|31\n' | monoblok --soundcheck patchbay.edn
printf 'sensors.temp|31\n' | monoblok --soundcheck --soundcheck-label patchbay.edn
```

With `--soundcheck-label` every output row is prefixed `in|` or `out|`, so
an input row with no `out|` row after it produced nothing. Feed at least one
row per rule, on a concrete subject that the rule's filter matches, and
include rows that should be suppressed as well as rows that should pass.

Things that behave differently under `--soundcheck`:

- Clock and window state lingers after EOF (10 seconds by default) so
  delayed forms (`debounce!`, `on-silence`, `aggregate!`, time bars) and
  `every` bodies can fire. `--soundcheck-linger-ms 0` exits at EOF, so none
  of those emit; use it for rules with no clocked forms, and set the linger
  just above the longest `:ms` when you need to see a delayed emit.
- All rows arrive within the same few milliseconds, so `:ms` windows see one
  burst: `rate` reads high and `hold-off` / `throttle` suppress everything
  after their first passes. Check wall-clock behaviour against a running
  server.
- `:reentrant true` publishes are followed as on the server, and `last`
  reads a table that honours the file's `(lvc ...)` filters.
- `reply-to` is always nil, so `reply!` publishes nothing. Check request-reply
  against a running server with `nats req`.
- Negative payloads are fine in rows (`sensors.temp|-5`). With `nats pub`,
  pass them after `--`: `nats pub sensors.temp -- -5`.

For Tinyblok/Patchbay Lite, prefer the firmware repo's `make soundcheck` or
`make test` because they build the host validator against the vendored
monoblok patchbay and the embedded `patchbay.edn`.

## Authoring checklist

Before returning a patchbay file, verify:

1. For monoblok, every top-level form is `(on ...)`, `(every ...)`, `(lvc ...)`, or a single optional `(export ...)` / `(import ...)`. Deprecated `(bridge ...)` is accepted as an export alias. For Tinyblok/Patchbay Lite, top-level forms are ordinary `(on ...)` rules plus `(pump ...)`, `(fn ...)`, and `(on-req ...)`; do not rely on `(lvc ...)`, `(export ...)`, deprecated `(bridge ...)`, or `(import ...)` there without alerting the user that they are beyond Lite.
2. Every `publish!` target is a concrete subject (no `*` or `>`, no `$LVC.` or `$STATS.` prefix - those are read-only).
3. Every numeric op is fed a numeric value (`payload-float` / `payload-int` / arithmetic result), not raw `payload`.
4. `N` in `moving-*` is a literal integer and consistent per call site, subject, and window kind.
5. Pipelines built with `->` end in a side-effecting form (`publish!`); a pipeline that ends in a pure value is dead code.
6. Tinyblok `(pump ...)` and `(fn ...)` declarations name registered native symbols, and `on-req` handlers reply with `reply!` rather than assuming arbitrary runtime subscriptions.
7. Comments start with `;` and never leak prose outside them.
8. Every form you used is documented in the relevant skill reference or verified against the target runtime. If the request needs something from "Forms that do not exist", say so instead of inventing a form.
9. You ran `monoblok --validate` and a `--soundcheck` with at least one row per rule (see "Cheap validation"), or told the user that you could not and gave them the commands.
