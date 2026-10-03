# Testing: THREE tiers, chosen by what changed — never two

There are THREE tiers, not two. Naming the tier you are running, out loud,
before you run it is part of the rule; "targeted or battery" is a false choice
and produces exactly the oscillation this rule exists to stop.

  TIER 1  TARGETED, per change      — seconds. `bash scripts/run_tests.sh
          [--exe --obj] <names>` over the new/affected tests plus the touched
          subsystem's neighbors, and the reducer with its gcc/clang oracle.
  TIER 2  FAST CONFORMANCE, per COMMIT that touches CODE_PATHS (src include
          third_party tests scripts tools examples) — UNDER THREE MINUTES,
          all of it: `bash scripts/fast_lanes.sh` (c-testsuite, c-torture,
          c2mir-tests, gui, index-c, gxx-c++11). It is a RATCHET against
          recorded baselines, it records each green lane in the ledger, and
          `scripts/lane_ledger.sh check --commit` reports its freshness.
          BATCH CHECKPOINT, per BATCH of fixes, never per fix (owner
          2026-09-28) — ~10 minutes: `bash scripts/batch_lane.sh` (the whole
          tests/ suite, JIT only; remote: `remote_build.sh sync build batch`,
          then record its tests-jit tally). Run it before reporting a batch
          done, before a push, and before moving to another area. Ledger row
          `tests-jit` (promote=batch): never blocks, `check` reminds while stale.
  TIER 3  THE SEAM BATTERY, per MERGE WAVE — about an hour, ONE command:
          `bash scripts/seam_battery.sh`, at the arc's release boundary only.
          Cheapest first: pre-build every toolchain + static gates, then
          `make -C src gates` (unit tests + gates, no suite), then the FULL
          tests/ suite ONE way — on the SHIPPED artifact, the -O2 packed
          madc-release, headerless (Linux and win64) — then the small on-disk
          subsets (`ondisk`, `ondisk-win`), then `exeobj` (--exe --obj on the
          packed binary), then the macOS build and aarch64 (owner 2026-10-02).
          The -O0 dev binary never runs the full suite at the seam.

  PER FIX = ONE command, Tier 1 + Tier 2: `scripts/fix_lanes.sh <globs>`
          (remote: `TESTS='<globs>' remote_build.sh sync build fix`). Never
          hand-chain the batch or the packed suite onto a fix.

TIER 2 IS NOT OPTIONAL AND IS NOT A SUITE. Three minutes is cheaper than the
targeted run it is being skipped in favour of. A change to `src/`, `include/`
or `third_party/` that has not run Tier 2 is not validated, however many
targeted tests are green: Tier 1 tests only what you thought to test, and
Tier 2 is what catches the rest. `/commit` (.claude/commands/commit.md) runs
Tier 2 for you — use it rather than deciding per commit.
Never defer Tier 2 to the seam "since the battery will cover it": the whole
point is that it runs while the change is still one change.

Incremental changes get TARGETED validation: the new/affected tests plus the
touched subsystem's neighbors — never the full battery per commit.
Run the seam battery ONCE per merge wave, at the release/merge gate
(or when a change's blast radius is genuinely suite-wide, e.g. lexer/include
machinery, shared codegen).
NEVER re-run a suite on content that is already green — ceremonies are git-only.
Draw the merge wave at FEATURE completion, not at every slice: BANK the whole
feature — every slice AND every known-open fix of it — before spending the
push-gate battery (gates, the packed headerless suite on Linux and win64, the
on-disk subsets, exe + obj, the macOS and aarch64 builds). A feature with a pending or
known-open fix is NOT a merge wave yet — hold the long suites until it is
complete, so they run once for the whole feature, not once per slice.
The merge wave is the SEAM the arc's plan or design doc names — its RELEASE
BOUNDARY (e.g. V1–V5 of the client-server arc) — never a slice, phase or "V"
below it, however complete that slice is on its own: a slice with its own plan
file and gates is still a slice. A pre-arc conversion slice, a design ruling and
a defect fixed on the way (own commit, TARGETED gate) all ride the seam battery.
Between seams the work banks on the arc's feature branch (pushes freely); the
ONE battery, the lane records and the develop merge/push happen at the seam.
Before launching the battery, name the seam it gates in one sentence; a sentence
that names a slice is a targeted run, not a battery.
The battery's `exeobj` stage covers native executables and objects on every
seam; do not leave the tree with JIT green and EXE broken, or EXE green and JIT broken.
Do NOT run integration tests in a shell loop — use the Makefile target.
A red test's history is the RELEASE ARCHIVE's: `scripts/release_bins.sh run
[--last N] [--head] <test>...` (build container) runs it on every archived
release; classify a red as regression vs pre-existing from that matrix.
A standing "pause before big test suites" instruction is about TIER 3. It never
covers Tier 1 or Tier 2 — if an instruction's scope is unclear, ask which tier
it means rather than silently promoting a three-minute gate into it.
