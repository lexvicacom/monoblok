# Advanced Patchbay Examples

These examples were built by Codex with `AGENTS_PATCHBAY.md` loaded.

They are intentionally wacky hallucinated demos, not reference deployments or
domain-accurate models. They are occasionally over the top, but they should
give you a feel for what is possible. Each example has a patchbay `.edn` file
and a matching Python pump script that publishes randomized telemetry into
monoblok.

Run an example by starting monoblok with the patchbay file first, then running
the matching pump script in another terminal.

## Cloudhook

Cloudhook is a fictional offshore fleet of autonomous kite turbines. The pump
publishes kite flight, weather, power, and log subjects under `cloudhook.*`.

```sh
monoblok cloudhook_patchbay.edn
python pump_cloudhook.py
```

## Moonwell

Moonwell is a fictional lunar ice-mining habitat. The pump publishes rig
environment, drill, power, and log subjects under `moonwell.*`.

```sh
monoblok moonwell_patchbay.edn
python pump_moonwell.py
```

The pump scripts default to `127.0.0.1:4222`. Use `--host` and `--port` if
monoblok is listening somewhere else.

## Making Your Own

To make more examples like these, give your coding agent the patchbay-specific
instructions first. In this repo, copy or append the contents of
`docs/AGENTS_PATCHBAY.md` into an `AGENTS.md` file in the directory where you
want the agent to work.

From the repo root, one direct way is:

```sh
cat docs/AGENTS_PATCHBAY.md >> advanced-examples/AGENTS.md
```

Then ask Codex, or another coding agent that reads `AGENTS.md`, for a complete
pair: one patchbay `.edn` file and one pump script that publishes matching test
data.

Example prompt:

```text
Give me a cool patchbay .edn file plus a matching Python pump driver script.

Make it a weird fictional telemetry domain, optionally with JSON input subjects, scalar
demux rules, alerts, stable dashboard outputs, audit subjects, and a few
windowed stats. The pump script should connect to monoblok on 127.0.0.1:4222
and publish realistic randomized data for the subjects the patchbay expects.
```

Once the agent writes the files, run the generated `.edn` first, then run the
generated pump script:

```sh
monoblok your_example_patchbay.edn
python pump_your_example.py
```
