# /save — Sync documentation and ensure all work is saved

Ensures all documentation is up to date with the current state of the codebase, then commits and pushes.

## Steps

1. **Check if a release is needed first**:
   - Run `git status` to check for uncommitted changes
   - Run `git log --oneline HEAD ^develop` to check for unmerged commits
   - If there are uncommitted changes OR commits not yet on develop, invoke the `/release` workflow first (read `.claude/commands/release.md` and follow it)
   - If develop is clean and up to date, skip the release step

2. **Sync claude_status.json**:
   - Read the current `claude_status.json`
   - Verify all fields reflect current state: version (from VERSION), test counts, phase statuses, features list, known issues, remaining TODOs
   - Run `make -C src fulltest` and update test_status with actual counts
   - Update the date to today
   - Write the updated JSON

3. **Sync TODO.md**:
   - Read current TODO.md
   - Verify items in High/Medium/Deferred sections are still accurate
   - Move any items that have been implemented (check git log since last release) to Completed
   - Remove duplicates between sections
   - Ensure Completed section has commit hashes where available

4. **Sync CHANGELOG.md**:
   - Read current CHANGELOG.md
   - If there's an `[Unreleased]` section with content, verify it matches recent commits
   - If the `[Unreleased]` section is empty, that's fine (means /release already captured everything)

5. **Sync docs/SMAUG_requirements.md**:
   - Read current file
   - Update the "Already Working" and "Not Yet Implemented" sections based on current feature state
   - Update the gap summary table severities (items that are now implemented should be marked)

6. **Sync README.md**:
   - Verify the test count in the Testing section matches actual (`make -C src fulltest` output)
   - Verify the Roadmap table reflects current phase statuses
   - Verify the Current Release section matches VERSION

7. **Scan docs/ for staleness**:
   - Read `docs/test-status.md` — verify test counts match
   - Check if any docs reference features or limitations that have been resolved
   - Flag any docs that need updating (don't rewrite them unless clearly wrong)

8. **Commit and push**:
   - If any files were modified, stage and commit: `Sync documentation for v{version}`
   - Push to current branch
   - If on develop, also push develop

9. **Report**: List what was updated, what was already current, and any docs flagged for manual review
