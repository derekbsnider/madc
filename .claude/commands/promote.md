# /promote — Promote develop to master (production release)

Merges develop into master, tags the release, and pushes.

## Steps

1. **Pre-flight checks**:
   - Ensure working tree is clean (no uncommitted changes) — abort if dirty
   - Ensure current branch is `develop` — if not, ask user to switch first
   - **Do NOT re-run test suites** (owner rule, 2026-08-09). A promotion
     inherits the release's validation: verify the promotion candidate is
     the SAME commit a recorded green battery ran at (the release's
     CHANGELOG/status/release-notes counts, or the release session's
     battery in this conversation). Promotion is a git ceremony, not a
     test event. Only if develop has moved SINCE the last validated
     release commit (code commits without a battery) does a fulltest run
     first — and then say why.
   - Read version from `VERSION` file

2. **Merge develop into master**:
   - Switch to `master`
   - Pull latest: `git pull origin master`
   - Merge develop: `git merge develop`
   - If there are merge conflicts, STOP and ask the user

3. **Tag the release**: `git tag vX.Y.Z` (from VERSION file)
   - If the tag already exists, skip tagging and warn the user

4. **Push to GitHub**:
   - Push master
   - Push tags: `git push --tags`
   - If `gh auth status` succeeds, publish the GitHub Release for madc:
     `gh release create vX.Y.Z --title "vX.Y.Z — <one-line theme>"
     --notes-file docs/release-notes/vX.Y.Z.md --latest`
     (if gh is not authed, note it in the report and continue)
   - Build and attach the distribution packages: run
     `bash scripts/package_release.sh` on the build container (it
     rebuilds in the distribution configuration, runs the packed suite
     against the exact packaged binary, and restores the tree), pull
     `dist/` back, then
     `gh release upload vX.Y.Z dist/madc_*.deb dist/madc-*.rpm dist/SHA256SUMS`

5. **Promote the MIR fork in lockstep** (`/workspace/mir` — branch
   correspondence: the fork's `master` tracks madc's `master`):
   - Verify the fork's `develop` is pushed and `MIR_COMMIT` points into it
   - Switch the fork to `master`, merge `develop` (expected fast-forward;
     if not, STOP and ask the user)
   - Push the fork's master and tags
   - Switch the fork back to `develop`

6. **Switch back to develop**

7. **Report**: Print confirmation with version number, tag, fork state, and GitHub URL
