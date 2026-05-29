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
   - Find the `## Roadmap` section
   - Add/update a `### Current Release` sub-section right before the Roadmap table with a paragraph summary of vX.Y.Z
   - Add/update a `### Recent Releases` sub-section with one-line summaries of the last 5 releases (read from `docs/release-notes/`)

8. **Update `claude_status.json`**:
   - Read the existing file and update all fields to reflect current state
   - Update the version, date, build_status, test_status, phases, features, known_issues, remaining_todo
   - Write the updated JSON

9. **Commit all changes on current branch**:
   - Stage: `VERSION`, `docs/plans/ROADMAP.md`, `CHANGELOG.md`, `README.md`, `claude_status.json`, `docs/release-notes/vX.Y.Z.md`, plus any other uncommitted changes
   - Commit message: `Release vX.Y.Z — <one-line summary>`
   - Do NOT use `--amend`

10. **Merge to develop** (if not already on develop):
    - Push the current branch to origin
    - Switch to `develop`
    - Merge the feature branch into develop (fast-forward or merge commit)
    - If there are merge conflicts, STOP and ask the user

11. **Push develop to GitHub**: `git push origin develop`

12. **Report**: Print a summary of what was released, the version number, and remind the user to run `/promote` when ready to push to master

## Important
- Run `make -C src fulltest` before starting — abort if tests fail
- Read the actual file contents before modifying — don't guess at current state
- Use today's date (from the system) for all date fields
- The commit must include the Co-Authored-By trailer
