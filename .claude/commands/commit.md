# /commit — commit with the right test tier already run

Argument: `$ARGUMENTS` — an optional subject or scope hint. Empty is fine; the
message is derived from the diff either way.

This command exists because the tier is the part that gets chosen wrong, not
the tests. See `.claude/rules/testing-fulltest.md`: there are THREE tiers, and
the failure mode is treating it as two and oscillating between hand-rolled
targeted runs and the multi-hour battery. **Do not decide the tier per commit.
Run this.**

## The tiers this command runs

- **Tier 1 — targeted.** Seconds. The new/affected tests plus the touched
  subsystem's neighbors, and the reducer with its gcc/clang oracle.
- **Tier 2 — fast conformance.** Under three minutes for all six lanes.
  `bash scripts/fast_lanes.sh`. Runs whenever the commit touches CODE_PATHS
  (`src include third_party tests scripts tools examples`).
- **Tier 3 — the seam battery.** NOT run here. It gates the merge wave
  (`/test`, the platform lanes), never a commit.

## Steps

1. **See what is actually being committed.** `git status --short` and
   `git diff --cached --name-only`. Stage deliberately — never `git add -A`,
   which sweeps up scratch files (the repo root routinely carries untracked
   `*.mad` experiments). Nothing staged: stage the files this change owns and
   say which they are.

2. **Classify the blast radius.** Intersect the staged paths with CODE_PATHS.
   A docs-only commit (`docs/`, `*.md`, `claude_status.json`, `CHANGELOG.md`)
   skips Tier 2 and goes to step 6. Anything under `src/`, `include/`,
   `third_party/`, `tests/`, `scripts/`, `tools/` or `examples/` runs it.

3. **Build.** `bash scripts/remote_build.sh sync build pull`. QNAP law: this
   shell is a NAS container — every build and every suite runs on the desktop
   container over `ssh -p 2299 dev@localhost`, never here. Read `build rc=`
   before believing anything downstream. Zero warnings is an owner law; the
   build is `-Werror`, so a warning is already a failure.

4. **Tier 1 — targeted.** `bash scripts/run_tests.sh [--exe --obj] <names>` on
   the container, over the new test plus its neighbors. A fix touching native
   artifact, AOT or shared codegen paths runs `--exe --obj` too. A new fix
   without a reducer in `tests/` carrying BOTH oracles is not ready to commit
   (`.claude/rules/fix-what-you-find.md`).

5. **Tier 2 — fast conformance.** On the container:
   `( ulimit -t 3600; timeout 2400 bash scripts/fast_lanes.sh )`, redirected to
   a log. It is a RATCHET: a test failing OUTSIDE its recorded baseline is a
   regression and **stops the commit**; a baseline test that now passes is LOUD
   and means shrink the baseline in the same commit. Do not pipe a long run to
   `head`/`tail` — it hides the exit status
   (`[[feedback_selfhost_lane_harness_traps]]`).
   **A red lane is not a reason to commit anyway and fix later.** The tier's
   whole value is finding it while the change is still one change.

6. **Write the message.** A commit touching `src/` or `include/` carries the
   four trailers, and `scripts/check-rule-trailers.sh` fails the build without
   them:
   `Hypothesis:` what you believed was wrong, written BEFORE editing ·
   `Layer:` the chain, and why the one you edited is deepest — if you cannot
   write it you are shimming, so stop and go lower ·
   `Searched:` the grep you ran, the CONCEPT (not the identifier already in
   your head), and what came back ·
   `Oracle:` what gcc/clang did on the reducer and what madc did.
   `n/a — <reason>` is allowed; silence is not. Attribution trailers per the
   session's instructions.

7. **Commit**, then **record the ledger**: `bash scripts/lane_ledger.sh record
   <lane> <tally>` for each lane that ran green, so
   `scripts/lane_ledger.sh check --commit` is fresh for the push gate. The row
   stamps the new HEAD, whose code content is what Tier 2 just tested.
   (`fast_lanes.sh` records automatically when it runs in the repo it is
   gating; when it ran on the container, copy the rows back.)

8. **Report by tier.** Name each tier, what it ran, and its tally. "Tests
   passed" without a tier is the report that let this go wrong.

## When NOT to use this

- A merge wave / release boundary: that is Tier 3 — `/test`, the platform
  lanes, `scripts/lane_ledger.sh check --promote`, then the merge.
- `third_party/mir` is ordinary madc source here, NOT a vendored dependency —
  it gets the same gates as `src/`.
