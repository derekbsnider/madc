# Branching Model — Why

Feature branches isolate new work from the stable `develop` branch. This was adopted after discovering that uncommitted changes across multiple files are fragile — a single bad `git checkout` can destroy everything.

The workflow: `feature/name` branch off `develop`, commit early and often, merge back via `--no-ff` when stable. This keeps the git history clean and makes it easy to compare feature vs baseline.

`develop` is the integration branch — all tests must pass. `master` is for releases only.

Now that work is handed back and forth between Claude Code and Codex CLI,
agent-owned feature branches reduce branch collisions and make unfinished work
safe to park. Suffixing branches with `-claude` or `-codex` makes ownership
obvious in `git branch` output and in hand-off notes:

- `feature/hashstr-next-claude`
- `feature/hashstr-next-codex`

The point is not permanent silos. It is to make WIP explicit. When one agent
hands a branch to the other, that hand-off should be recorded in the session
note and in the updated status files so both tools agree about who owns the
next edit.

## Why the promote gate is "all class-(a) green", not "match asmjit's 1645"

The original gate said develop must match or exceed master's asmjit-backend
torture count (1645/1685). The 2026-06-11 failset classification audit
(`docs/parity/failset-classification.md`, user-signed) showed that number no
longer measures the right thing:

- The two backends' capability sets diverged — CIR passes 34 tests asmjit
  fails, asmjit passes 82 CIR fails — so comparing raw counts compares
  different feature sets, not progress toward correctness.
- Classifying every remaining failure found 33 tests that exist only to
  exercise gcc internals, gcc-only extensions with no real-world use, or
  undefined behavior where madc's choice is as defensible as gcc's (clang
  diverges from gcc on several of them too). Chasing those is parity
  theater, and the user ruled 100% torture parity is explicitly not the
  goal.
- What actually gates promotion is standard-C correctness: the 41 class-(a)
  failures are real compliance bugs (dominated by K&R old-style definitions
  and implicit-decl handling — exactly the C89 SMAUG-class material madc
  exists for). Hence: gate = all class-(a) fixed = ≥1608 of the 1652
  in-scope tests, class-(c) formally skipped, class-(b) on the roadmap.

## Why the lane-freshness push gate (2026-08-28)

- Three lanes drifted silently in one month: SMAUG was red five weeks in
  every shipped release (no lane compiled the artifact), the wine lane
  accumulated 75 red tests from a July 24 emission change (the lane last
  ran ~v0.92.1), and macOS went five releases with nothing darwin even
  compiled (the MT arc's rt_task.c would not build there at first try).
- The common failure: "promote tests everything" was a checklist, not a
  mechanism — a rule without a gate decays. The ledger makes the claim
  mechanical: a lane is FRESH iff no code content (src/ include/
  third_party/ tests/ scripts/ tools/ examples/) changed since its
  recorded green commit, so docs-only commits never stale a lane and any
  code change stales every lane that has not re-run.
- The gate rides the PUSH of develop/master (owner directive), not just
  /promote, because develop is the integration branch other work builds
  on — a red lane behind a pushed develop propagates.
- The check runs its own negative control first (a stale row must block,
  a fresh row must pass): a gate that cannot fail is not a gate. The
  selftest is IN-PROCESS — its first version re-entered the script via
  "$0" and fork-spiraled the QNAP into a hard reset (2026-08-28); the
  tombstone comment in lane_ledger.sh marks the lesson.

## Why every platform lane's FULL suite gates master (owner law 2026-09-04)

- The first ledger gated what ran; it did not ask whether what ran was a
  TEST of the platform. The `macos` row was the container cross build plus
  pack verification — a build lane — and the CI "Mac battery" is a ten-test
  smoke. The full darwin suite ran only when the owner dispatched
  darwin-probe.yml by hand, and its residue (9 aarch64 MIR-floor tests, 3
  libc++ `<list>` tests) lived in a plan document, not in the gate.
- The libc++ flavor lane (macOS's library, run on linux hardware) was not in
  the ledger at all. Its last full run was 2026-08-16 (1046/0/13skip); the four
  std::list tests landed 2026-08-18; the three libc++ `<list>` failures were
  visible only on the darwin runner. Nothing on linux had exercised libc++
  `<list>` under madc, and the lane's green was remembered as covering it.
- genuine Windows was `promote=no` ("flip when automatable") — an opt-out
  by omission from the one gate that exists.
- The ledger header said "/promote runs it as a hard gate"; the /promote
  command did not. A checklist again, in a comment.
- The mechanism: a second check mode. `check --promote` (the develop push)
  gates `promote=yes` rows; `check --release` (the master push and /promote)
  gates `yes` AND `release` rows. The release tier holds the platform suites
  whose cost or hardware keep them off every develop push:
  `darwin-suite` (darwin-probe with `suite_gate=true`, both arches),
  `genuine-win`. (`libcxx`, the libc++ flavor run on linux hardware, was in
  this tier until 2026-10-03.)

## Why the libc++ lane is the darwin suite (owner 2026-10-03)

- Each platform has one stdlib flavor and each packed binary freezes one:
  Linux and Windows libstdc++, macOS libc++. A binary packing both was ruled
  out.
- The linux `libcxx` lane ran the -O0 dev binary with no libc++ pack, so
  every C++ test parsed the libc++ headers live — about 5.5x its libstdc++
  time on one `<vector>` test — and it invoked the runner three times (plain,
  `--exe`, `--obj`), each repeating the JIT pass: over two hours for
  coverage the darwin suite already gives.
- darwin-probe runs the FULL suite with `MADC_BIN=bin/madc-release-<arch>-macos`
  — the shipped -O2 libc++ packed binary — and `MADC_SKIP_EXT` includes
  `libcxx`, so every `.libcxx_skip` fixture applies there. That is the
  libc++ configuration users run.
- `-stdlib=libc++` on Linux stays a supported flag, gated by
  `scripts/libcxx_gate.sh` in fulltest. An unrecorded row (`none`) is stale by definition, so master
  is blocked until each has run green on the candidate. The selftest carries
  the third negative control: a stale release-tier row must pass `--promote`
  and block `--release`.
- Consequence stated plainly: the darwin residue is a promotion blocker. Each
  failing test is fixed, or skipped with a `.darwin_skip` whose one line says
  why it is structurally out of the domain — the class-(c) convention of the C
  torture gate — before a master release.

## Why `/dupaudit` runs in `/commit`, not before a merge

Owner ruling, 2026-09-22: *"this dupaudit gate should be part of the /commit
not part of the merge gate because it results in re-writing code."*

The audit's findings are rewrites. At the merge, the seam battery has already
run, so any consolidation the audit drives there is code the battery never
tested: either the battery runs a second time for it, or untested code merges.
Both are the wrong answer. At `/commit` the same rewrite is still part of ONE
change, and Tier 1 and Tier 2 — which `/commit` runs AFTER the audit — validate
it in minutes.

It is also where the audit finds the most. New copies are born in the commit
that writes them; an audit at the merge finds a copy weeks after it was
written, when the author has moved on. The first merge-time run (2026-09-22,
the self-hosting wave) found two divergent families — the MIR conversion
builtins still carrying the bootstrap cycle on four non-x86 targets, and a
`void`-pointee predicate unguarded at two sites — that each belonged to the
commit that fixed the first instance, not to the merge.

Scope it to the diff. A per-commit audit of the whole subsystem would be the
Tier 3 mistake again; what changes per commit is the concepts the diff touches.
An older family found on the way is recorded, not folded into the commit.

## Why every release must shrink an existing family

Owner ruling, 2026-10-16: *"with each release we need to ensure that we are
actively seeking to reduce code duplication, and not only preventing additional
duplication."*

The per-commit audit and the family gates only stop NEW copies. The families
that already exist keep their copies, and each copy keeps drifting. The day of
the ruling showed the cost: "is a bare function name a call or its address" is
answered separately by the identifier arm's follower whitelist, the static
member arm and the function-pointer arm, so in C `f ? a : b` decayed in one
position and became a call in another, and `!f` called `f` — a silent wrong
answer. The fix that day could only gate C; the family itself still has three
owners.

So the reduction is a planned piece of each release arc, not a side effect: a
whole-subsystem `/dupaudit` sweep (recon, which `/dupaudit` already allows on
request), then at least one existing family consolidated in its own commits
with the usual Tier 1 + Tier 2, before the seam battery — so the battery runs
over the consolidated code, as the merge-gate ruling above requires. The
`Consolidated:` line in the release's CHANGELOG section is the evidence, and
`scripts/check-release-consolidation.sh` makes a release without it fail.
Releases cut before the ruling (up to v0.105.0) are exempt by its EPOCH, which
moves forward only.


## Why every release must move C23 or C++20 forward (owner law 2026-10-16)

Owner ruling, 2026-10-16: *"we also need to have a release gate that each
release must move the needle forwards towards C23 and/or C++20 compliance."*

The north star is C23 / C++23 parity. A release that only adds tooling, IDE
work or dialect features can be entirely worthwhile and still leave the
standards where they were, and a run of such releases drifts away from the
goal without anyone deciding it should. A rule stated without a gate decays,
so the law has the same shape as the duplication law above: evidence in the
release's CHANGELOG section, checked by a script `/release` runs.

The evidence is measured, never typed. `docs/conformance-coverage.md` holds
figures taken by hand on older content (its C23 rows on 0.103.0), and a
hand-typed count is exactly the number that goes stale. So the counts live in
a ledger that one script writes, `scripts/conformance_measure.sh`, over four
corpora of gcc's own tests: the C23 tests compile-clean and run
(`gcc.dg/c23-*`), the execute torture under `--std=c23`, and `g++.dg/cpp2a`
under `--std=c++20`. The gate compares a release's row with the previous
release's, corpus by corpus.

Two rows compare only when they measured the same thing. The ledger carries
each corpus's in-scope count, and the gate refuses a pair whose counts differ:
a newer gcc testsuite on the build host, or a runner whose scope rules
changed, makes a different corpus, and the fix is to re-measure the previous
release on its archived binary (`tmp/release-bins/`, kept for exactly this
kind of comparison), not to compare across. Both sides are release binaries —
the archived one and the battery's madc-release — so the -O0 development
build never stands in for what ships.

The rule asks for one count up and none down. A gain in one corpus bought
with a loss in another is a trade, not progress, and the gate names the fall.
v0.105.0, the last release before the ruling, is the baseline: its EPOCH
exempts it and every earlier release, and moves forward only.

## Why a patch release is held only to "lowers none" (owner 2026-10-24)

Both laws above are about a release ARC: the weeks of work between two minor
releases, where room exists to consolidate a family and move a standard. A
patch release (x.y.Z, Z > 0) is not an arc. It ships regression fixes on top
of a release already out — v0.106.1 carried two: the Windows release build's
missing host madc, and the pack writer's report on every Windows run from a
dialect host — and making it wait for a conformance gain or a consolidation
would keep a known regression in users' hands to satisfy a rule written for
something else. So a patch release consolidates nothing and needs no rise, but
it is still measured and may lower no count: a fix that breaks a standard is
not a fix. Both gate scripts read the version's last component and carry a
patch control each way.

## Why every release is compared with the previous release first (owner 2026-10-24)

v0.106.0 ran three days past its plan and still needed v0.106.1. Every lane
had been green, and every lane compared the candidate with a recorded
baseline or an oracle, never with the release before it. What slipped was
what only that comparison shows:
- a program's stderr gaining a line (the pack writer's report on every
  Windows run from a dialect host);
- execution benchmarks moving by code placement alone (fannkuch +18% on
  identical instructions, B232), chased by hand for days because nothing told
  a placement mover from a real one.

`scripts/release_diff.sh` puts both binaries on one machine, interleaved. It
covers startup, execution, compile time, peak memory, callgrind cost, the
whole tests/ suite with its saved stdout and stderr, and the package member
lists. It runs FIRST in the release arc, because every long suite after it is
wasted on a candidate that is slower or noisier than what users already have.

The shift test makes the placement question mechanical. A mover is rebuilt at
five code placements by putting padding functions ahead of the source, then
averaged. A slowdown that averages out is placement. One that survives is
real.

An intended change, such as a deliberate new warning or a package that
dropped a file on purpose, is excused for that version alone, with its reason,
in `docs/parity/release-diff-allow.tsv`. An excuse that carried forward would
hide the next regression of the same row.

## Why a release without its callgrind map cannot reach master (owner 2026-10-16)

Owner ruling, 2026-10-08: keep a callgrind map for every release. The map —
each parse-cost workload's full profile on a symbolized build of the release
commit, and the top functions by self and inclusive cost — is what turns a
red `parse-cost` row into an answer: diffing two releases' maps names the
functions that grew. Without the previous release's map, a regression found
at the next seam has to be bisected by rebuilding old commits.

The directive was a step to remember, and it was forgotten: v0.103.0 and
v0.105.0 shipped without maps, and the first parse-cost regression after
v0.105.0 (2026-10-16) had no v0.105.0 map to diff against. So the step is
gated the way the release archive already is: `release_bins.sh check`, which
the master push gate runs after its selftest, refuses an archived release
since v0.102.1 (the first one mapped) that has no `callgrind/summary.tsv`,
and the selftest proves an entry without a map fails. `/release` step 13
makes the map beside the archive entry, while the release commit is fresh.
