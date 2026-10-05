# Testing Fulltest — Reasoning and Gotchas

See `.claude/rules/testing-fulltest.md` for the rule itself.

## Why fulltest moved from per-change to per-merge-wave (owner, 2026-08-19)

The rule originally read "fulltest after every change." In practice that
compounded into multiple full batteries plus release-lane ceremonies in a
single day (v0.89/0.90/0.91 each ran fulltest + exe + packed + headerless),
which the owner called out directly: it "wastes time and cpu and disk
grinding away with thousands of tests for every little change" — the fifth
repetition of the same feedback. The battery's job is to keep `develop`
stable at the point work MERGES, not to re-prove every incremental commit;
a targeted run (the new gates plus the touched subsystem's tests) catches
the same regressions at a fraction of the cost, and the full battery still
runs once, where it counts. The blast-radius exception exists because some
layers (lexer include machinery, shared codegen) genuinely touch every
test — there a targeted run cannot bound the risk.

## Why the merge wave is drawn at feature completion (owner, 2026-09-07)

"Battery once per merge wave" only saves time if the merge wave is a COMPLETE
feature. The push-gate battery is hours of cross-platform suites (fulltest +
exe + obj + packed + headerless, then c-testsuite, wine, and the macOS cross
build). Launch it on a feature that still has a known-open fix and the moment
that fix lands the lane ledger marks every lane stale ("code content changed")
and the whole battery runs AGAIN — the same hours twice.

The owner drew this line after a battery was kicked off with one of a feature's
three live-window bugs (the web-window resize-fill) still unbanked: "I prefer to
try to get a feature completed before running hours and hours of test suites,"
and "we established to try to bank a bunch of fixes before running the 2-hour+
long full test suites." So bank the whole feature — every slice, every
known-open fix — THEN run the long suites once.

This is a judgment rule, not a mechanical gate: nothing can detect "feature
complete" for you. The lane ledger's freshness check is the cost signal it
minimizes — every content change after a green lane re-stales it, so the
cheapest path is to make the content whole before the first expensive run.

## Why the seam is the release boundary, not a slice (owner, 2026-09-09)

The 2026-09-07 rule said "feature completion", and two days later the same
cost arrived through a relabelling: slice V0.5 of the client-server arc (the
enums-not-strings conversion) had its own plan file and its own gates, so it
was framed as "a complete feature" and given the push-gate battery — while the
arc's design doc already said the local half V1–V5 "is one feature and gets ONE
merge-wave battery when complete". The battery then ran three times over
(each defect it surfaced re-staled every lane), the double cost the 2026-09-07
section predicted, on a slice that was never the seam.

The owner's words: "every phase/slice/V/whatever you call them does not need a
full test suite ... that comes at the seam ... we weren't supposed to have all
these tests until V5 was complete ... and you ran them at V0.5."

So the unit is not "a feature" as the agent happens to draw it; it is the SEAM
the arc's plan names — its release boundary. The arc's slices bank on the
arc's feature branch (`feature/<arc>-claude`), which the pre-push hook lets
push freely; a defect found on the way gets its own commit and its targeted
gate (a forest fix runs the forest gates, not the battery) and rides the seam.
At the seam: one battery, the lane records, the develop merge and push. The
one-sentence test before any battery launch — "this gates <arc>'s release
boundary <Vn>, every slice banked" — exists because the relabelling is silent:
nothing in the tooling can tell a slice from a seam, only the plan can.

## Why the seam battery runs the suite ONCE, on the packed -O2 artifact (owner, 2026-10-02)

Until 2026-10-02 the seam ran the whole tests/ suite four ways: `fulltest`
and `exe` + `obj` on the -O0 dev binary, then `packed` and `headerless` on
the -O2 packed `madc-release`. The develop battery that day measured the cost
(Linux): fulltest 28 min, exe + obj 42 min, packed 3.5 min, headerless 3 min.
About 70 of its 85 minutes were the -O0 binary compiling, and the packed and
headerless runs re-ran the same 1900 tests on the shipped binary in under four
minutes each.

The battery logs since late July (tmp/logs/rb-*.log) show which lane caught
what:

- packed or headerless caught failures the -O0 run did not, repeatedly:
  testvolatilepointeeo2 (packed only, 09-24); header roots missing from the
  pack (headerless only, 09-24 and 10-02); a forest-bind regression that made
  73 `.expect_quiet` tests fail under the packed binary while fulltest was green
  (10-02); testprojectmtiorder (packed and headerless only, 10-02).
- the -O0 run caught nothing the packed runs missed in the two full
  batteries compared (09-24: its 2 failures were among packed's 3; 10-02: 0).
- exe/obj caught about one defect a month after the July AOT bring-up
  (testcompoundlitdesig 08-29; testnexus_layers 09-15, structural; the
  testgraphpast timeout fixture 09-20).
- on win64, headerless-win's failures always contained wine's (8 vs 13 on
  09-24, 19 vs 20 on 10-02).

So the owner's ruling: the full barrage runs ONE way, on the binary that
ships, headerless (the strictest form: nothing on disk can rescue a forest
decline). The on-disk include path rides only the tests whose
`.headerless_skip` fixtures exclude them from that run (`headerless_suite.sh`
complement mode), so the two runs together cover the suite once. exe + obj
stay a FULL run — they are the lane that sees emission and runtime-linking
defects — but on the packed binary, where compile time no longer dominates
(a 28-test subset: 5 s packed vs 16 s dev). The order is cheapest-first, so a
red stage is seen in minutes. The -O0 dev binary keeps the per-fix and
per-batch tiers, where its incremental build is the point.

`make -C src fulltest` still exists (gates + the suite on the dev binary) for
a local all-in-one run; `make -C src gates` is the seam's gate stage.

## Why native-EXE work needs an explicit second lane

`make -C src fulltest` validates the normal JIT-backed path. It does
not prove that `save_executable()` / standalone ELF execution still
works. Native-AOT bugs routinely hide behind green JIT results because
they stress different surfaces:

- ELF patching and relocation
- standalone helper export/linkage
- aggregate return ABI at real call boundaries
- startup/runtime state reconstruction
- global data materialization

So the seam battery's `exeobj` stage runs `--exe --obj` over the whole
suite on every merge wave, not only when the work looks native.

## Why the rule forbids leaving one lane broken

The SMAUG work made this concrete: "JIT green" was not enough. A small
aggregate return bug left the EXE lane unable to create a character
cleanly even though the ordinary suite was passing. The repo should not
accept that kind of partial validation as "good enough" anymore.

## Why the rule now names THREE tiers (owner, 2026-09-22)

The rule described two tiers — targeted-per-change and battery-per-merge-wave —
and said, in its first sentence, "never the full battery per commit." The fast
conformance tier existed the whole time: `scripts/fast_lanes.sh`, under three
minutes for six lanes, with its own `--commit` gate in `scripts/lane_ledger.sh`
and its own `promote=commit` column in `docs/lane-status.tsv`. It was named in
**none** of `.claude/rules/` — the files an agent actually loads.

So the only two answers available to the question "what do I run?" were
hand-rolled targeted tests or the multi-hour battery. The owner described the
result exactly: *"you seem to either run super long-assed 3 hour test suites
for every tiny thing, or you only run hand-rolled tests for a huge pile of
changes... so random."*

It is not randomness. It is a two-valued rule applied to a three-valued
reality, and it will recur for any agent until the rule says three.

The session that triggered this is the worked example. Five fixes landed —
two of them in `third_party/mir` (every unsigned→floating conversion MIR
generates) and one in `c2mir` (every float→`uint64_t` cast in every C program
compiled) — validated with 16 targeted tests, hand reducers against gcc and
clang, and a 426-program self-host differential. All green, all real, and all
Tier 1. `c_torture_lane.sh` — 1612 standard-C programs, four minutes, the lane
whose own header says it "belongs in the FAST tier that runs after every
commit" — was classified as a "major test suite" and held for the seam, along
with the rest of `fast_lanes.sh`. The branch was pushed five times without it.

That misclassification had a second source worth recording: a session-scoped
owner instruction to "pause before initiating any major test suites" was read
as covering `fast_lanes.sh`. An instruction about Tier 3 was applied to Tier 2.
Hence the last line of the rule: a pause instruction is about the battery, and
if its scope is unclear, ask which tier it means.

## Why the gate is at PUSH and not at COMMIT

The obvious mechanism — a pre-commit hook that refuses a code commit until the
fast tier is green — cannot work, and the reason is structural. The ledger
records a lane green against `git rev-parse HEAD`, and freshness is
`git diff --quiet <recorded> HEAD -- $CODE_PATHS`. Before you commit, HEAD is
still the *previous* commit, so the first code commit of any change always
reads stale, and the gate would block every code commit unconditionally.

The push is where the model works: commit freely, and before the work leaves
the machine the fast tier must be green on current content. The pre-push hook
already enforced this for `develop` and `master` (`gate_applies --promote`
already admits `promote=commit` rows). The hole was that feature branches
pushed freely — which is precisely the hole the 2026-09-22 session fell
through. The hook now applies the `--commit` tier to every branch, and the
develop/master tiers on top of it where they applied before. Docs-only pushes
stay free, because staleness is measured over `CODE_PATHS`.

`/commit` (`.claude/commands/commit.md`) is the proactive half: it runs Tier 2
on the working-tree content BEFORE committing, so the gate is satisfied by the
time the push happens, and a red lane is found while the change is still one
change.

## Why a BATCH checkpoint runs the tests/ suite (owner, 2026-09-28)

The fast tier is fast because it leaves out the one suite madc's own features
are tested in: `tests/*.mad`. Its lanes are external conformance corpora
(c-testsuite, c-torture, c2mir, index-c, the g++.dg subset, the CommonMark spec) plus the GUI
directory. Between seams, nothing ran `tests/` except each fix's own Tier 1
selection, and Tier 1 tests only what the author thought to test.

The 2026-09-28 BUGS.md burn-down made the cost visible. About fifty fix
commits, every one Tier 1 and Tier 2 green, and the first full JIT run of
`tests/` afterwards found four regressions: a defaulted `vector(vector &&)`
refused and then moved twice (returned vectors came back empty), a returned
`var` literal constructed through `var(const char *)`, and a libc++
`string_view` conversion that c2mir refused. None were in any fix's
neighbourhood, and all four were in the suite. The run takes under ten
minutes (557 s on the desktop container, 2026-09-28).

Per fix, ten minutes would turn the burn-down back into the oscillation the
three tiers exist to stop, which is why the owner drew it at the BATCH: after a
run of fixes, before the batch is reported done, pushed, or left for another
area. JIT only: the exe and obj passes double and triple the cost and belong to
the seam, where the battery runs all three.

The ledger row (`tests-jit`, promote=`batch`) never blocks. A per-commit block
would make it per fix, and the develop push already requires the battery,
whose fulltest runs the same suite. What it does is speak: `lane_ledger.sh
check` prints a BATCH reminder while the row is stale, and `/commit` runs that
check, so the reminder is in front of whoever is committing.

## Why the per-fix gate is one named command (owner, 2026-10-01)

During a bug run (B112, a non-deduced-conversion regression, B111), every fix
was validated by a hand-composed chain: Tier 1, Tier 2, the batch, `make -C src
release` and the packed suite, about thirty minutes per fix. The rule already
said the batch runs once per batch of fixes and the memory file said so too;
the drift happened because the per-fix run was re-typed each time, and each
re-typing appended whatever had last been useful. The owner: "we certainly
cannot be running a 30+ minute barrage of tests for each individual bug fix",
and, on hearing the rule was already written down, "maybe we need a command
for this". `scripts/fix_lanes.sh` (and `remote_build.sh`'s `fix` stage) is that
command: Tier 1 over the fix's globs on JIT, exe and obj, then Tier 2, about
five minutes, with a zero-test Tier 1 counted RED because a wrong glob looks
green otherwise. A rule restated in prose decays; a command that does exactly
the per-fix work, and nothing more, is the mechanism.

## Why a red test is classified against the release archive (owner, 2026-10-02)

The release rule (fix regressions, file pre-existing defects) turns on one
question per red test: did a shipped release pass it? Answering it by hand
meant building an old tag in a scratch tree, or trusting a hand-off claim.
`tmp/release-bins/` already held every release binary for timing comparisons,
but the archive step was a prose step in `/release`, and it decayed twice
over: the NAS copy stopped at v0.97.0, and the container's v0.98.0–v0.99.2
copies were thin drivers (bin/madc-release has been a 150 KB driver over
libmadc.so.0 since v0.98.0) archived without their libmadc, so each bound the
host's libmadc and died on a symbol lookup. One copy, labelled v0.81.0,
reported 0.80.0 (baked before the VERSION bump).

`scripts/release_bins.sh` is now the archive's one owner: each entry is a
self-contained directory (the binary, its lib/, a PROVENANCE note) verified
where it runs; `backfill` recovers a missing release from the shipped tarball
or by building its release commit; `run` is the regression matrix. The master
push gate (`lane_ledger.sh check --release`) refuses a release missing from
the archive, after a negative control proves the check can fail.

