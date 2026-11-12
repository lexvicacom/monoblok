# Sample live NATS input

Use a bounded sample when the user asks to inspect a NATS feed to inform Patchbay
rules. Establish the subject filter and connection target from the request. Accept
a CLI "profile" as a NATS **context**, passed explicitly with `--context NAME`.
Ask for missing target information rather than guessing a server or subscribing
to `>` by default. Use the user's requested count; otherwise sample up to 100
messages over at most 30 seconds.

## CLI and connection

Check `command -v nats` and `nats sub --help` before sampling. The user must have
the [NATS CLI](https://github.com/nats-io/natscli#installation) installed on the
machine running the agent. If absent, explain the prerequisite and let the user
install it; do not substitute a client library or download it automatically.

Use an existing context so authentication and TLS settings remain in the user's
CLI configuration. Do not read or print context files, request credential values,
or copy secrets into commands or generated rules. Do not change the selected
default context. If the user explicitly wants the current default, omit
`--context`; for an explicitly supplied unauthenticated URL, use `--server URL`
with `--no-context`. Do not put credentials in a URL.

## Capture a bounded sample

For example, for profile `plant-readonly`, filter `sensors.>`, and 100 messages:

```sh
nats --context plant-readonly sub 'sensors.>' --count=100 --wait=10s --timestamp
```

Quote wildcard filters. Use normal subscription output to retain concrete
subjects, payloads, headers, and reply subjects when present; `--raw` loses
subject information. Check the installed CLI's help if an option is unavailable.

`--count` bounds the total messages across matching subjects. `--wait` is an
**idle** timeout, not a total capture deadline; global `--timeout` is not a
subscription deadline either. Enforce the total time limit with the execution
tool's process timeout or a managed process that you interrupt at the deadline.
Retain partial output when time expires and stop the process before proceeding.
Do not rely on GNU `timeout` being installed on macOS. Stop early if output is
too large to inspect usefully, and report the truncation.

Use a plain Core NATS subscription: no queue group, JetStream flags, durable
consumer, acknowledgements, requests, or probe publishes. This observes new
traffic without competing with queue workers or advancing a stored consumer.
There is no historical replay. A quiet feed can yield fewer than the requested
count, including zero; report that rather than inventing payloads or broadening
the filter automatically. Report connection or permission errors without dumping
credentials or repeatedly retrying.

## Turn observations into rule inputs

Summarize the context/target, filter, requested and observed counts, capture
duration, and whether capture ended by count, idle timeout, deadline, or error.
Describe concrete subject patterns and payload formats, representative JSON
field paths and types, numeric/text encodings, missing/null fields, arrays, and
observed variants. Distinguish observations from assumptions; a sample does not
establish a complete schema, rare cases, units, or reliable timing guarantees.
Do not infer timing from a truncated transcript.

Show only the representative payload excerpts needed to explain the shape,
redacting secrets and personal values. Keep raw captures temporary unless the
user asks to save fixtures. For binary payloads, report that they need a known
decoder rather than guessing a schema.

Use representative observed subjects and payloads for validation/soundcheck
inputs. Add synthetic boundary and suppression cases clearly labeled as such.
Sampling supplies input shape; obtain desired output subjects and behavior from
the user before inventing transformations. A connection context authenticates
the CLI only: do not embed it or its credentials into Patchbay configuration.
