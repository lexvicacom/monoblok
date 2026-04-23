# How monoblok uses AI

There's no shame in using Claude or any of the other assistants, but
monoblok is not a vibe-coded project and I'd rather be upfront about
where the line sits.

Short version: I use Claude Code to help review diffs and sanity-check
benchmarks. I don't use it to invent numbers or make design calls.
Here's the longer version so nobody has to guess.

## The tool

Everything AI-shaped in this repo goes through [Claude
Code](https://claude.com/claude-code) running **Claude Opus 4.7 (1M
context)**. That's it. No Copilot, no Cursor agents, no local models,
no fine-tunes. If I switch models, I'll update this file in the same
commit.

## What I actually use it for

**Reviewing my own diffs.** Zig 0.16 is fiddly, the arena-vs-gpa split
in the rule evaluator is easy to mess up, and libxev has sharp edges.
Before I push anything non-trivial I ask Claude to read the diff
against `main` and flag correctness bugs, lifetime mistakes, dead
branches, missing tests, or places where the change disagrees with
`CLAUDE.md`. The `/review` and `/security-review` skills are the
usual entry points. I read the output, disagree with bits of it,
and fix the real findings.

**Sanity-checking benchmarks.** monoblok has `scripts/smoke.sh`,
`scripts/bridge-smoke.sh`, and a pile of ad-hoc perf runs under
`--release=fast`. I'll ask Claude things like "is this benchmark
actually measuring what I think it is", "am I warming up enough",
"does this look like coordinated omission". I also use it to turn
raw numbers into readable prose for release notes and blog posts.

**Learning Zig as it moves.** Zig is a moving target and 0.16 broke a
lot of things I thought I knew (`std.Io`, `std.posix`, file I/O, the
shape of the networking backends). Claude is genuinely useful as a
second pair of eyes on "is this the 0.16 way to do it or am I holding
onto a 0.14 habit", and honestly as a tutor when I'm relearning
something from scratch. That's part of the value for me as an
engineer, not just a code-review helper.

**Memory layout and C integration.** The hot path cares about struct
layout, alignment, where allocations actually live, and what survives
an arena reset. The nats.c bridge adds another layer: hand-written
bindings, ownership rules across the FFI boundary, which calls are
thread-safe versus which need the loop thread. Claude is a useful
check on "does this cast do what I think", "who owns this pointer
after the call", "is this `extern struct` laid out the way the C
header expects". I still read the C headers myself, but talking it
through shortens the loop.

**Sounding board for daft ideas.** A lot of what's in monoblok started
as "what if the patchbay could do X", "what if `$LVC` also did Y",
"what if the bridge were bidirectional". Most of those ideas are bad
and get killed in the conversation, which is the point. The ones that
survive get prototyped by me, not by the model. Treating Claude as a
rubber duck that pushes back is probably where I get the most value
out of it day to day.

A caveat on all of the above: these models are trained to be helpful,
which shades into sycophancy more often than is useful. Nothing Mr.
Claude says is taken as gospel. He (they?) gets sworn at frequently,
told he's wrong, and asked to try again. If a suggestion survives
that, it's probably worth something.

The other half of not-taking-it-as-gospel is actually reading the
docs and the prior art. The Zig stdlib source, Andrew's release
notes, mitchellh's libxev (and the rest of his Zig work), the
TigerBeetle codebase, the nats.c headers, and the rest of the small
pile of genuinely excellent Zig code floating around on GitHub all
get consulted directly. If Claude and the real source disagree, the
real source wins, every time.

## What I don't use it for

- **Merging code I haven't read.** The model can write a patch, but
  nothing hits `main` until I've read every line of the diff and run
  `zig build test` plus the smoke scripts locally. "Claude said it
  looked good" is not a merge criterion.
- **Design decisions.** Single-threaded libxev, the arena/gpa split,
  hand-written nats.c bindings instead of `@cImport`, export-only
  bridge, etc. Those are mine, with the reasoning written down in
  `CLAUDE.md`. The model reads that file; it doesn't get to rewrite it.
- **Making up numbers.** Every throughput or latency figure in this
  repo came out of an actual run of the compiled binary on a real
  machine. If you see a number, it was measured.
- **Security or production-readiness claims.** monoblok is a toy (the
  README says so at the top) and I'm not going to have a chatbot tell
  you otherwise.

## Traceability

- Commits where Claude materially helped carry a `Co-Authored-By:
  Claude ...` trailer. Commits without it are just me.
- When a review pass finds a real bug, the fix commit mentions it so
  the provenance shows up in `git log`.
- This file is the source of truth for which model is in use. If
  you're reading an old revision, check this file at that revision,
  not the current one.

## Why bother writing this

The interesting parts of monoblok are the bits bolted onto the
NATS-shaped core (the patchbay DSL, `$LVC.*` last-value streams,
`$STATS.*`, the export-only bridge) and the fact that it holds up
performance-wise against the real thing on a single thread. Some of
that may turn into something less toy-shaped at some point, who knows.
Either way, if the additions are the pitch and the perf numbers are
the evidence, you should know which bits a model touched and which it
didn't. The design of those additions is mine; the numbers are
measured; Claude's job is to catch the bugs I wrote on the way there.

If something in the code, the docs, or a benchmark seems to contradict
any of this, open an issue and I'll sort it out.
