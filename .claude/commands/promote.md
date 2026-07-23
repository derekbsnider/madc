# /promote — Promote develop to master (production release)

Merges develop into master, tags the release, and pushes.

## Steps

1. **Pre-flight checks**:
   - Ensure working tree is clean (no uncommitted changes) — abort if dirty
   - Ensure current branch is `develop` — if not, ask user to switch first
   - Run `make -C src fulltest` — abort if tests fail
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

5. **Promote the MIR fork in lockstep** (`/workspace/mir` — branch
   correspondence: the fork's `master` tracks madc's `master`):
   - Verify the fork's `develop` is pushed and `MIR_COMMIT` points into it
   - Switch the fork to `master`, merge `develop` (expected fast-forward;
     if not, STOP and ask the user)
   - Push the fork's master and tags
   - Switch the fork back to `develop`

6. **Switch back to develop**

7. **Report**: Print confirmation with version number, tag, fork state, and GitHub URL
