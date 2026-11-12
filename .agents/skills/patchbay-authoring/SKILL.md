---
name: patchbay-authoring
description: Write, edit, review, or port Monoblok and Tinyblok/Patchbay Lite rule files, including EDN, YAML, JSON, validation, soundchecks, and matching sample traffic. Use for Patchbay DSL work, rather than general daemon C development.
---

# Patchbay authoring

Help the user express message routing and conditioning as valid Patchbay rules.
Honor the requested files and format; use EDN when none is specified. For a
file-only response, put explanations in comments appropriate to that format.
For reviews and explanations, use ordinary prose. Keep companion files separate.

## Establish the target and inputs

For file creation, use the output filename supplied by the user or clearly
implied by their prompt or target file. If no filename can be inferred, ask for
one before saving; do not silently choose `patchbay.edn`. Continue any independent
analysis while awaiting the answer. Preserve the existing path when editing.

Infer Monoblok daemon versus Tinyblok/Patchbay Lite from the project and request.
Ask only when the runtime is ambiguous and changes the answer; otherwise use the
portable subset. Identify input subjects, representative payloads, output subjects,
thresholds, windows, and timing. State material assumptions in the result.

Read [the language reference](references/language.md) before authoring or reviewing
rules. It covers grammar, bindings, forms, state, threading, gating, and common
mistakes. Read additional references only when relevant:

- [Tinyblok](references/tinyblok.md): firmware, native pumps/functions, request handlers, and Lite limitations. Check this before using daemon features in firmware.
- [YAML and JSON](references/formats.md): format spellings and ports. In a Monoblok checkout, also read `docs/patchbay-yaml-schema.md` before YAML work; it is the canonical schema.
- [Monoblok features](references/monoblok.md): LVC/`last`, request-reply, timers, export, import, and JetStream replay.
- [Worked examples](references/examples.md): rule composition when an example would help.
- [Pump drivers](references/pump-drivers.md): only when the user requests a host-side traffic generator; adapt the bundled template to the actual input subjects.

Use the target checkout's documentation and supported forms when they differ
from this portable package. Do not invent operators or silently substitute a
different runtime. Explain when a requested operation belongs upstream or downstream.

## Keep the model visible

- Lists are calls and vectors are data. There is no Clojure compatibility layer,
  quote, macro, or `let`.
- `->` threads into the last argument. Gates return nil on suppression;
  `publish!` does nothing with nil. Guard `last` against a missing cached value.
- State is per call site and concrete subject. To combine subjects in Monoblok,
  funnel them onto one subject; consider the number of state slots a wildcard creates.
- Emissions re-enter rules only from a rule marked `:reentrant true`. Use it for
  deliberate stages and keep funnel outputs outside their input filters.
- Use `payload-float`, `payload-int`, or another numeric value for numeric forms.
  An empty payload is not zero. Edges need a prior value before they fire.
- Use explicit effects such as `publish!`, concrete output subjects, and the
  user's thresholds. Keep raw imported messages private unless a rule republishes them.
- Tinyblok has different request semantics and fewer features. Do not introduce
  JSON forms unless its optional JSON support is explicitly enabled.

## Validate the result

For authored or changed files, follow [validation and soundcheck guidance](references/validation.md).
Use `./build/monoblok` in a Monoblok checkout, or `monoblok` on PATH elsewhere.
Check for an existing executable before downloading anything. If Monoblok is
missing, follow the reference's download procedure on Linux or macOS and use
the downloaded binary for validation. For Windows users, briefly apologise that
native Windows binaries are not supported and suggest WSL with Linux; do not
run the downloader in native Windows.
For Tinyblok, prefer the firmware checkout's host validator and test targets.

Run `--validate` and a representative `--soundcheck` when the target tools are
available. Include passing, suppressed, and boundary cases for the changed rules.
Use an appropriate linger for clocked forms; soundcheck cannot establish real
wall-clock or request-reply behavior. For format ports, compare the same rows
against both versions.

Show the relevant transcript and material assumptions. If tools are unavailable,
say which checks were not run and provide the commands. Validation establishes
well-formedness, and soundcheck covers only its supplied cases; do not claim
broader behavior from either. For a review-only request, report findings without
rewriting files unless requested.
