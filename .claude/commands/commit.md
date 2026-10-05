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

1. **See what is actually being committed.** `git status --short` and
   `git diff --cached --name-only`. Stage deliberately — never `git add -A`,
   which sweeps up scratch files (the repo root routinely carries untracked
   `*.mad` experiments). Nothing staged: stage the files this change owns and
   say which they are.

2. **Classify the blast radius.** Intersect the staged paths with CODE_PATHS.
   A docs-only commit (`docs/`, `*.md`, `claude_status.json`, `CHANGELOG.md`)
   skips the audit and Tier 2 and goes to step 7. Anything under `src/`, `include/`,
   `third_party/`, `tests/`, `scripts/`, `tools/` or `examples/` runs it.

3. **Audit the diff for semantic duplication** (`/dupaudit`, scoped to THIS
   diff — `.claude/commands/dupaudit.md`). It runs HERE, before the build,
   because its findings rewrite code: a consolidation made now is part of the
   change Tier 1 and Tier 2 are about to validate. For every helper, predicate
   or rule the diff adds or edits, grep the CONCEPT for sibling
   implementations and re-check the KG `DupFamily` rows whose scope overlaps.
   A copy THIS change creates or diverges from is adopted/consolidated before
   committing; an older family found on the way is recorded (`open`), not
   folded into this commit (`fix-what-you-find.md`: its own commit).

4. **Build.** `bash scripts/remote_build.sh sync build pull`. QNAP law: this
   shell is a NAS container — every build and every suite runs on the desktop
   container over `ssh -p 2299 dev@localhost`, never here. Read `build rc=`
   before believing anything downstream. Zero warnings is an owner law; the
   build is `-Werror`, so a warning is already a failure.

5–6. **Tier 1 + Tier 2 — ONE command.**
   `TESTS='<glob> [glob...]' bash scripts/remote_build.sh sync build fix`
   (scripts/fix_lanes.sh on the container, ~5 minutes): Tier 1 runs the globs
   — the new test plus its neighbors — on JIT, exe and obj in one run; Tier 2
   runs `scripts/fast_lanes.sh` under its CPU and wall caps. It prints one
   tally line per tier and ends `fix_lanes: GREEN` or `RED`. **Run this, never
   a hand-chained sequence:** chaining is how the batch and the packed suite
   crept into every fix (owner 2026-10-01). A Tier 1 that matches no test is
   RED (a wrong glob). A new fix without a reducer in `tests/` carrying BOTH
   oracles is not ready to commit (`.claude/rules/fix-what-you-find.md`).
   Tier 2 is a RATCHET: a test failing OUTSIDE its recorded baseline is a
   regression and **stops the commit**; a baseline test that now passes is LOUD
   and means shrink the baseline in the same commit. Do not pipe a long run to
   `head`/`tail` — it hides the exit status
   (`[[feedback_selfhost_lane_harness_traps]]`).
   **A red lane is not a reason to commit anyway and fix later.** The tier's
   whole value is finding it while the change is still one change.
   **Nothing else runs per fix:** the batch is step 10's, once per batch of
   fixes; the packed and platform lanes are the seam battery's.

7. **Write the message.** A commit touching `src/` or `include/` carries the
   four trailers, and `scripts/check-rule-trailers.sh` fails the build without
   them:
   `Hypothesis:` what you believed was wrong, written BEFORE editing ·
   `Layer:` the chain, and why the one you edited is deepest — if you cannot
   write it you are shimming, so stop and go lower ·
   `Searched:` the grep you ran, the CONCEPT (not the identifier already in
   your head), and what came back ·
   `Oracle:` what gcc/clang did on the reducer and what madc did.
   `n/a — <reason>` is allowed; silence is not. Write each field as a terse
   factual claim, never a first-person investigation narrative — the gate wants
   the claim, and the narrated form trips an output-side "reasoning extraction"
   classifier (`.claude/rules/rule-trailers.md`). Same for the CHANGELOG entry:
   behaviour facts, no discovery narrative. Attribution trailers per the
   session's instructions. On a 5-series session (Opus/Sonnet/Fable 5.x) the
   classifier trips on the trailers even when terse, and it fires as soon as the
   message / four facts / CHANGELOG prose appear in this model's OWN turn — a
   heredoc writing the message file and the Agent-tool `prompt` are both that.
   So composing the facts here and then delegating is too late. Either run dev
   on a pre-5 model (Opus 4.8) and `git commit -F` inline (preferred, verified),
   or, on a 5.x session, write none of that prose in your turn: stage, then
   spawn a `model: haiku` subagent whose prompt gives only pointers — "compose
   the trailers + CHANGELOG from `git diff --cached` and reducer `tests/<name>`,
   then commit" — and verify the landing with `git log -1`. See
   `.claude/rules/rule-trailers.md`.

8. **Commit**, then **record the ledger**: `bash scripts/lane_ledger.sh record
   <lane> <tally>` for each lane that ran green, so
   `scripts/lane_ledger.sh check --commit` is fresh for the push gate. The row
   stamps the new HEAD, whose code content is what Tier 2 just tested.
   (`fast_lanes.sh` records automatically when it runs in the repo it is
   gating; when it ran on the container, copy the rows back.)

9. **Report by tier.** Name each tier, what it ran, and its tally. "Tests
   passed" without a tier is the report that let this go wrong.

10. **Close the batch.** When this commit ends a BATCH of fixes (you are about
   to report the batch done, push, or move to another area), run the batch
   checkpoint: `bash scripts/remote_build.sh sync build batch`, then record
   its printed tally with `bash scripts/lane_ledger.sh record tests-jit
   "<tally>"`. It is the whole tests/ suite, JIT only, about ten minutes,
   never per fix. `check --commit` prints a BATCH reminder while
   `tests-jit` is stale (`.claude/rules/testing-fulltest.md`).

## When NOT to use this

- A merge wave / release boundary: that is Tier 3 — `/test`, the platform
  lanes, `scripts/lane_ledger.sh check --promote`, then the merge.
- `third_party/mir` is ordinary madc source here, NOT a vendored dependency —
  it gets the same gates as `src/`.
