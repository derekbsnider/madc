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
- **Tier 2 — fast conformance.** Under three minutes for all seven lanes.
  `bash scripts/fast_lanes.sh`. Runs whenever the commit touches CODE_PATHS
  (`src include third_party tests scripts tools examples`).
- **Tier 3 — the seam battery.** NOT run here. It gates the merge wave
  (`/test`, the platform lanes), never a commit.

## Steps

1. **See what is actually being committed.** Run `git status --short` and
   `git diff --cached --name-only`. Stage deliberately — never `git add -A`,
   which sweeps up scratch files (the repo root routinely carries untracked
   `*.mad` experiments). If nothing is staged, stage only the files belonging
   to this change and report which files were staged.

2. **Classify the blast radius.** Intersect the staged paths with CODE_PATHS.
   A docs-only commit (`docs/`, `*.md`, `claude_status.json`, `CHANGELOG.md`)
   skips the audit and Tier 2 and goes to step 7. Anything under `src/`,
   `include/`, `third_party/`, `tests/`, `scripts/`, `tools/` or `examples/`
   runs it.

3. **Audit the diff for semantic duplication** (`/dupaudit`, scoped to THIS
   diff — `.claude/commands/dupaudit.md`). It runs HERE, before the build,
   because its findings can rewrite code: a consolidation made now is part of
   the change Tier 1 and Tier 2 are about to validate.

   For every helper, predicate, or rule the diff adds or edits, grep for the
   underlying CONCEPT and inspect sibling implementations. Re-check the KG
   `DupFamily` rows whose scope overlaps.

   A copy THIS change creates or causes to diverge must be
   adopted/consolidated before committing. An older duplication family found
   during the audit is recorded as `open`, not folded into this commit
   (`fix-what-you-find.md`: it gets its own commit).

4. **Build.** Run:

   `bash scripts/remote_build.sh sync build pull`

   QNAP law: this shell is a NAS container — every build and every suite runs
   on the desktop container over `ssh -p 2299 dev@localhost`, never here.

   Check the reported `build rc=` before accepting downstream results.
   Zero warnings is an owner law; the build uses `-Werror`, so a warning is
   already a failure.

5–6. **Tier 1 + Tier 2 — ONE command.**

   `TESTS='<glob> [glob...]' bash scripts/remote_build.sh sync build fix`

   This invokes `scripts/fix_lanes.sh` on the container (~5 minutes):

   - Tier 1 runs the selected globs — the new test plus its neighbors — on
     JIT, exe, and obj.
   - Tier 2 runs `scripts/fast_lanes.sh` under its CPU and wall caps.

   It prints one tally line per tier and ends with either
   `fix_lanes: GREEN` or `fix_lanes: RED`.

   **Run this command rather than a hand-chained sequence.** Hand chaining is
   how the batch and packed suites crept into every fix (owner 2026-10-01).

   A Tier 1 glob matching no test is RED and indicates an invalid test
   selection.

   A new fix without a reducer in `tests/` carrying BOTH required oracles is
   not ready to commit (`.claude/rules/fix-what-you-find.md`).

   Tier 2 is a RATCHET:

   - A test failing outside its recorded baseline is a regression and
     **stops the commit**.
   - A baseline test that now passes is LOUD and requires shrinking the
     baseline in the same commit.

   Do not pipe a long run through `head` or `tail`; doing so can hide the
   relevant exit status (`[[feedback_selfhost_lane_harness_traps]]`).

   **A red lane stops the commit.** Resolve the failure while the change is
   still isolated.

   **Nothing else runs per fix:** the batch belongs to step 10 and runs once
   per batch of fixes; packed and platform lanes belong to the seam battery.

7. **Write the message.**

   A commit touching `src/` or `include/` carries four required trailers, and
   `scripts/check-rule-trailers.sh` fails the build when they are absent:

   `Hypothesis:` concise statement of the observed incorrect behavior and the
   expected correction.

   `Layer:` component or subsystem responsible for the behavior, plus the
   repository/design fact establishing that ownership.

   `Searched:` search command or concept examined, together with the relevant
   result.

   `Oracle:` observed gcc/clang result for the reducer and the corresponding
   madc result.

   `n/a — <reason>` is allowed; an omitted field is not.

   These fields are **operational records, not an investigation transcript**.
   Write each as a short, verifiable claim based on the diff, repository
   structure, commands run, test output, or compiler output.

   Do not include private reasoning, internal deliberation, step-by-step
   thought process, or a narrative of how a conclusion was reached.

   The CHANGELOG follows the same rule: record externally observable behavior
   and implementation results, not an investigative narrative.

   Attribution trailers follow the session's normal instructions.

8. **Commit**, then **record the ledger**:

   `bash scripts/lane_ledger.sh record <lane> <tally>`

   Record each lane that ran green so
   `scripts/lane_ledger.sh check --commit` is fresh for the push gate.

   The row stamps the new HEAD, whose code content is what Tier 2 validated.

   `fast_lanes.sh` records automatically when it runs in the repository it is
   gating; when it ran on the container, copy the rows back.

9. **Report by tier.** Name each tier, what it ran, and its tally.

   Do not report only "tests passed"; include the tier so the scope of the
   validation is explicit.

10. **Close the batch.** When this commit ends a BATCH of fixes — before
    reporting the batch complete, pushing, or moving to another area — run:

    `bash scripts/remote_build.sh sync build batch`

    Then record its printed tally with:

    `bash scripts/lane_ledger.sh record tests-jit "<tally>"`

    This is the complete `tests/` suite in JIT mode, approximately ten
    minutes, and runs once per batch rather than once per fix.

    `check --commit` prints a BATCH reminder while `tests-jit` is stale
    (`.claude/rules/testing-fulltest.md`).

## When NOT to use this

- A merge wave / release boundary: that is Tier 3 — `/test`, the platform
  lanes, `scripts/lane_ledger.sh check --promote`, then the merge.
- `third_party/mir` is ordinary madc source here, NOT a vendored dependency —
  it gets the same gates as `src/`.
