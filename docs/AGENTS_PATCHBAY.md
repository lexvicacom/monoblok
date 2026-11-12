# Patchbay authoring skill

Use the `patchbay-authoring` skill to write, edit, review, or port rules for
Monoblok or Tinyblok/Patchbay Lite. It includes the language reference, runtime
differences, EDN/YAML/JSON spellings, worked examples, validation instructions,
and a host-side pump template. It does not require a Monoblok checkout.

The package uses the open Agent Skills format supported by Codex and Claude Code.
Its description is available for discovery; the instructions and relevant
references load when the agent uses the skill. Keep repository development
invariants in `AGENTS.md` or `CLAUDE.md` rather than appending this language guide.

## Install without a checkout

Download [patchbay-authoring.zip](https://raw.githubusercontent.com/lexvicacom/monoblok/main/site/downloads/patchbay-authoring.zip).
The archive contains a `patchbay-authoring/` directory with `SKILL.md`, references,
and assets. Extract the whole directory, not just `SKILL.md`.

Run these commands in the project where you want to author patchbays. Choose the
installation for your agent:

```sh
curl -fL https://raw.githubusercontent.com/lexvicacom/monoblok/main/site/downloads/patchbay-authoring.zip \
  -o /tmp/patchbay-authoring.zip

# Codex: project installation
mkdir -p .agents/skills
unzip /tmp/patchbay-authoring.zip -d .agents/skills

# Claude Code: project installation
mkdir -p .claude/skills
unzip /tmp/patchbay-authoring.zip -d .claude/skills
```

For a personal installation available across projects, extract into
`~/.agents/skills` for Codex or `~/.claude/skills` for Claude Code instead.
Install in one scope per agent to avoid duplicate entries. To update, download
the current archive and replace the installed `patchbay-authoring` directory.
Restart the agent if the skill does not appear.

The ZIP is rebuilt from the skill source whenever the docs site is built. For a
version-pinned copy, use a checkout at the desired release or commit instead.

## Use the skill

In Codex, mention `$patchbay-authoring`. In Claude Code, invoke
`/patchbay-authoring`. Both agents can also select it automatically when the
request matches its description.

For example, in Codex:

```text
$patchbay-authoring
Target Monoblok. Write patchbay.edn for sensors.*.temp with numeric payloads.
Round to one decimal place and publish only changed values to <subject>.stable.
Validate it and show a soundcheck with repeated, changed, and negative readings.
```

For Claude Code, use the same brief after `/patchbay-authoring`. Specify
Tinyblok/Patchbay Lite when targeting firmware, and request YAML or JSON when
wanted; EDN is the default.

Give the agent concrete input subjects, representative payloads, desired outputs,
and thresholds or windows. Ask for an explanation or review in ordinary prose;
ask for a file-only response when you want only the configuration text.

If you need to discover the input shape, install the
[NATS CLI](https://github.com/nats-io/natscli#installation) on the agent's machine
and ask it to sample live messages. Supply a subject filter, message count, and
an existing CLI profile (NATS calls these **contexts**), so credentials stay in
your CLI configuration:

```text
$patchbay-authoring
Use NATS CLI profile plant-readonly to sample up to 100 messages from sensors.>
for at most 30 seconds. Summarize the subjects and payload shapes, then use those
inputs to write sensors.edn, rounding temperature readings to one decimal place
and publishing changed values to <subject>.stable. Validate and soundcheck it.
```

The agent passes the profile with `nats --context plant-readonly sub ...` without
changing your default context. Sampling observes new Core NATS traffic; it does
not replay JetStream history or consume a durable consumer. A quiet feed may
yield fewer messages. The skill requires an installed `nats` command for this
optional workflow and does not install it automatically.

Install [Monoblok](https://github.com/lexvicacom/monoblok#run-it) on the same machine
so the agent can run `monoblok --validate` and `monoblok --soundcheck`. Inside a
Monoblok checkout it uses `./build/monoblok`; Tinyblok projects should use their
firmware host validator. The skill itself installs no binary or dependencies.
If tools are unavailable, the agent should say so and give you the check commands.

Read generated rules before deploying them. Validation checks well-formedness;
a soundcheck transcript covers only the messages supplied. Check real timing
and request-reply behavior against a running target when those matter.

## Use from a checkout

The canonical package is
[`.agents/skills/patchbay-authoring`](../.agents/skills/patchbay-authoring/SKILL.md).
Codex discovers it in this repository. Claude Code discovers the same package
through `.claude/skills/patchbay-authoring`, a relative symlink. No global install
is needed for work in this tree.

To install from a checkout into another project, copy the whole canonical
`patchbay-authoring` directory into that project's `.agents/skills/` or
`.claude/skills/`. Copy the canonical directory rather than the Claude symlink,
which points back into this repository.

For a tool without skill discovery, extract the ZIP and ask it to read
`patchbay-authoring/SKILL.md`, following the relevant reference links while
working on your rules. This file is now the usage guide; the former standalone
prompt has moved into the skill's references.

See [Codex skill documentation](https://learn.chatgpt.com/docs/build-skills)
and [Claude Code skill documentation](https://code.claude.com/docs/en/skills)
for discovery locations and invocation details.
