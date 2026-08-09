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

7. **Update README.md**:
   - There is exactly ONE `## Current Release` section — REPLACE its
     paragraph with the vX.Y.Z summary (search the whole file for stray
     older "Current Release" headers first; never add a second one)
   - Update the "Branch state" line in that section if branch facts changed
   - Update the `### Recent Releases` sub-section with one-line summaries of the last 5 releases (read from `docs/release-notes/`)
   - Update the bold "Current status" test-counts line near the Testing section

8. **Release the MIR fork alongside** (only if the fork changed since the
   last madc release — compare the commit in `MIR_COMMIT` against the commit
   the previous `v<MIR_VERSION>` fork tag points to):
   - New fork version: `<upstream-base>-madc.<new madc version>` — upstream
     base = the newest upstream MIR release merged into the fork (bump it
     only when a newer upstream release was merged; otherwise carry it
     forward from the current `MIR_VERSION`)
   - Write the new version to `MIR_VERSION` (included in the release commit)
   - On the fork (`/workspace/mir`): ensure `develop` is pushed and
     `MIR_COMMIT` points at it, then
     `git tag -a v<MIR_VERSION> <MIR_COMMIT commit> -m "MIR fork release <MIR_VERSION>"`
     and push the tag
   - If `gh auth status` succeeds, also publish the GitHub Release:
     `gh release create v<MIR_VERSION> --repo derekbsnider/mir --title
     "<MIR_VERSION> — the MIR madc vX.Y.Z ships against" --notes-file <notes> --latest`
     (notes: what changed in the fork since its previous release, +
     "Consumed by madc vX.Y.Z"). If gh is not authed, note it in the
     report and continue.
   - If the fork is UNCHANGED since the previous release: leave
     `MIR_VERSION` untouched, cut no fork tag

9. **Update `claude_status.json`**:
   - Read the existing file and update all fields to reflect current state
   - Update the version, date, build_status, test_status, phases, features, known_issues, remaining_todo
   - Write the updated JSON

10. **Commit all changes on current branch**:
    - Stage: `VERSION`, `MIR_VERSION` (if bumped), `docs/plans/ROADMAP.md`, `CHANGELOG.md`, `README.md`, `claude_status.json`, `docs/release-notes/vX.Y.Z.md`, plus any other uncommitted changes
    - Commit message: `Release vX.Y.Z — <one-line summary>`
    - Do NOT use `--amend`

11. **Merge to develop** (if not already on develop):
    - Push the current branch to origin
    - Switch to `develop`
    - Merge the feature branch into develop (fast-forward or merge commit)
    - If there are merge conflicts, STOP and ask the user

12. **Push develop to GitHub**: `git push origin develop`

13. **Archive the release binary** (owner directive 2026-08-09): after the
    release rebake, copy the packed release binary to the per-release archive
    on BOTH hosts so releases can be timed against each other later:
    `cp -p bin/madc-release tmp/release-bins/madc-release-vX.Y.Z`
    (`tmp/` is gitignored; create `tmp/release-bins/` if missing). Do the same
    in the container tree when the rebake happens there.

14. **Report**: Print a summary of what was released, the madc version, the fork release (if one was cut), and remind the user to run `/promote` when ready to push to master

## Important
- Run `make -C src fulltest` before starting — abort if tests fail
- Read the actual file contents before modifying — don't guess at current state
- Use today's date (from the system) for all date fields
- The commit must include the Co-Authored-By trailer
