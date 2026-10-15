# /release — Prepare and publish a release

Argument: `$ARGUMENTS` (one of: `major`, `minor`, `patch`, or empty for `minor`)

Perform the full release workflow:

## Steps

1. **Read the current version** from `VERSION` file (semver: MAJOR.MINOR.PATCH)

2. **Determine the new version** based on the argument:
   - `major` → increment MAJOR, reset MINOR and PATCH to 0
   - `minor` (default) → increment MINOR, reset PATCH to 0
   - `patch` → increment PATCH

3. **Write the new version** to `VERSION`

4. **Update docs/plans/ROADMAP.md**:
   - Move any newly completed items (with `~~strikethrough~~`) from High/Medium sections to the Completed section, if they haven't been moved yet
   - Keep the Completed section sorted chronologically (newest at bottom)

5. **Update CHANGELOG.md**:
   - Change the top `[Unreleased]` header to `[vX.Y.Z] — YYYY-MM-DD` with today's date
   - Add a brief one-line summary after the version header describing the theme of this release
   - Add a new empty `## [Unreleased]` section above it for future changes

6. **Write `docs/release-notes/vX.Y.Z.md`**:
   - Title: `# Release vX.Y.Z — YYYY-MM-DD`
   - A 2-3 sentence summary paragraph
   - Bullet-point highlights of the most important changes (max 10 bullets)
   - Link to the full changelog: `See [CHANGELOG.md](../../CHANGELOG.md) for full details.`

7. **Update README.md** — its version facts only (owner 2026-10-15: the
   README presents madc; release narrative lives in `CHANGELOG.md` and
   `docs/release-notes/`, test counts in `docs/test-status.md`):
   - The `## Releases` line: "The current release is **X.Y.Z**."
   - The `## Speed` table's opening sentence ("madc X.Y.Z against …") ONLY
     when the timings were re-measured for this release — never re-label
     old numbers with a new version.
   - Never add a "Current Release" / "Recent Releases" section, a branch
     state line or test counts.

8. **MIR needs no separate release** (subtree migration 2026-08-11):
   MIR lives in-tree at `third_party/mir`, so the madc release version
   identifies the exact MIR it ships — there is no `MIR_VERSION`, no
   fork tag, no lockstep. If the release notes should call out notable
   MIR-level changes, read them from `git log -- third_party/mir`.

9. **Update `claude_status.json`**:
   - Read the existing file and update all fields to reflect current state
   - Update the version, date, build_status, test_status, phases, features, known_issues, remaining_todo
   - Write the updated JSON

10. **Commit all changes on current branch**:
    - Stage: `VERSION`, `docs/plans/ROADMAP.md`, `CHANGELOG.md`, `README.md`, `claude_status.json`, `docs/release-notes/vX.Y.Z.md`, plus any other uncommitted changes
    - Commit message: `Release vX.Y.Z — <one-line summary>`
    - Do NOT use `--amend`

11. **Merge to develop** (if not already on develop):
    - Push the current branch to origin
    - Switch to `develop`
    - Merge the feature branch into develop (fast-forward or merge commit)
    - If there are merge conflicts, STOP and ask the user

12. **Push develop to GitHub**: `git push origin develop`

13. **Archive the release binary** (owner directive 2026-08-09, 2026-10-02):
    after the release rebake, on the build container,
    `bash scripts/release_bins.sh archive` — it files the tree's
    `bin/madc-release` WITH its `lib/` (the driver is thin since v0.98.0) as
    `tmp/release-bins/vX.Y.Z/`, after checking the tree holds the release's
    content and the entry runs. Then on the NAS
    `bash scripts/release_bins.sh sync` mirrors it. The master push gate
    (`scripts/lane_ledger.sh check --release`) refuses a release missing from
    the archive.

14. **Report**: Print a summary of what was released and the madc version, and remind the user to run `/promote` when ready to push to master

## Important
- **Do NOT re-run test suites on already-validated content** (owner rule,
  2026-08-09; fifth repeat 2026-08-11 — this file was the missed copy of
  the fix that went into promote.md). A release commit is docs-only
  (VERSION, CHANGELOG, README, status, notes): if every commit since the
  last recorded green battery is docs/status-only, that battery IS the
  release gate — cite it and move on. Only if CODE (`src/`, `include/`,
  `tests/`, `scripts/` runners) changed since the last green battery does
  a suite run first — via `scripts/remote_build.sh` (the container; the
  NAS never builds), and then say why.
- Read the actual file contents before modifying — don't guess at current state
- Use today's date (from the system) for all date fields
- The commit must include the Co-Authored-By trailer
