# How monoblok uses AI

Short version: I use Claude Code. I can't imagine not using it. I give them credit on relevant commits.

There's no shame in using Claude or any of the other assistants, but there's some backlash around its tasteful use - no doubt due to the volume of slop out there. **This is not a vibe-coded project.**

[This is an interesting area](https://github.com/melissawm/open-source-ai-contribution-policies) for sure.

>Did I write every line of code? **No,** it's 2026. Do I understand the architecture? **Yes,** I thought of it - whether it's good or bad.

The valuable/interesting parts of monoblok are the **ideas** - the the bits bolted onto the
NATS-inspired core (the patchbay DSL, `$LVC.*` last-value streams,
`$STATS.*`, the export-only bridge) and the fact that it sort of holds up
performance-wise against the real thing on a single thread.

Claude is just a tool to help make the ideas happen quickly (fail fast), write boring code and catch bugs. 

That said, if you see any AI BS or suspected overly-verbose slop, let me know. _You're absolutely right.__

## Where it works best

**Code review** Zig 0.16 is fiddly, the arena-vs-gpa split
in the rule evaluator is easy to mess up, and libxev has sharp edges.
Before I push anything non-trivial I ask Claude to read and flag correctness bugs, lifetime mistakes, dead
branches, missing tests, or places where the change disagrees with
`CLAUDE.md`.

**Sanity-checking and doing first pass benchmarks.** monoblok has `scripts/smoke.sh`,
`scripts/bridge-smoke.sh`, and a pile of ad-hoc perf runs under
`--release=fast`. I'll ask Claude things like "is this benchmark
actually measuring what I think it is", "am I warming up enough",
"does this look like coordinated omission". Claude automates this kind of non-core busy work that you would expect from a good codebase.

**Keeping up with Zig as it evolves (and breaks stuff).** Zig is a moving target and 0.16 broke a
ton of things. Sorry, changed them. Claude is genuinely useful at schooling me on the new way.

**Talking me out of daft ideas.** A lot of what's in monoblok started
as "what if the patchbay could do X", "what if `$LVC` also did Y",
"what if the bridge were bidirectional". Most of those ideas are distractions
and get killed in the conversation, which is the point. The ones that
survive get prototyped by me. Treating Claude as a
rubber duck that pushes back is priceless, keeping me on the straight and narrow. That said, this repo exists so maybe it doesn't always get listened to :)

**First stab at docs** AI is well-placed to give the skeleton/framework of documentation, which again, might get neglected in favour of writing code. Both are important. AI can get dry, verbose and repetitive so this is something I aim to avoid.

Sycophancy: nothing Mr. Claude, Esq says is taken as gospel. He (they?) gets sworn at frequently,
told he's wrong, and asked to try again. If a suggestion survives
that, it's probably worth something.

The other half of not-taking-it-as-gospel is actually reading the
docs and the prior art. The Zig stdlib source, release
notes, mitchellh's libxev (and the rest of his Zig work), the
TigerBeetle codebase, the nats.c headers, and the rest of the small
pile of genuinely excellent Zig code floating around on GitHub all
get consulted directly. If Claude and the real source disagree, the
real source wins, every time.

## What I don't use it for
- **Ideas.** Claude hasn't got domain experience and cannot connect the dots in a "you know what would be nice here..." way that a human can.
- **Design decisions.** Single-threaded libxev, the arena/gpa split, export-only
  bridge, etc. Those are mine, with the reasoning written down in
  `CLAUDE.md`. The model reads that file; it doesn't get to rewrite it.
- **Making up numbers.** Every throughput or latency figure in this
  repo came out of an actual run of the compiled binary on a real
  machine. If you see a number, it was measured.
- **Security or production-readiness claims.** monoblok is an experiment.

