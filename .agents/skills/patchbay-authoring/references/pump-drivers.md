# Host-side pump drivers

## Companion pump drivers

This section is for host-side monoblok examples. For Tinyblok/Patchbay Lite,
do not create a Python pump driver unless explicitly asked; use top-level
`(pump ...)` declarations backed by compiled C symbols and register those
symbols in Tinyblok's native symbol table.

If you are asked to generate a pump driver for the topology, create a
small standalone script that publishes realistic sample traffic into
monoblok. Prefer Python with only standard-library imports. The driver
should:

- connect to `127.0.0.1:4222` by default, with `--host` and `--port`
- speak basic NATS over TCP: read `INFO`, send `CONNECT` and `PING`,
  wait for `PONG`, then send `PUB` frames
- publish exactly the input subjects the patchbay expects, not the
  derived output subjects
- emit JSON objects when the patchbay uses `json-demux!` or `json-get`
- include `--interval`, `--count`, `--seed`, and a useful `--verbose`
- run until interrupted when `--count 0`
- avoid external dependencies and avoid using a NATS client library

Use [the pump template](../assets/pump.py) as a starting point; replace its example subject and payload with the actual inputs.

