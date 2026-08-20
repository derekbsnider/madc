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

4. **Write the MASTER release notes** (owner rule, 2026-08-20):
   - A promotion's notes summarize EVERYTHING since the PREVIOUS MASTER
     release (`git describe --tags --abbrev=0 master` before the merge),
     never just the last develop-to-develop delta — humans reading the
     GitHub release see only master releases.
   - **FEATURES FIRST, prominently**: the important and interesting
     capabilities a user gains, in plain language, each with a one-line
     example where it helps. Bugfixes, internals, infrastructure and
     process changes go in a compact section AT THE BOTTOM.
   - Source material: every CHANGELOG section between the two master
     tags. Group related increments into ONE feature entry (e.g. a
     feature and its follow-up releases are one story).
   - Save as `docs/release-notes/vX.Y.Z-master.md`, committed on develop
     before the merge. The per-develop-release notes files stay as they
     are — this file is the promotion-facing summary.

5. **Push to GitHub**:
   - Push master
   - Push tags: `git push --tags`
   - If `gh auth status` succeeds, publish the GitHub Release for madc:
     `gh release create vX.Y.Z --title "vX.Y.Z — <one-line theme>"
     --notes-file docs/release-notes/vX.Y.Z-master.md --latest`
     (if gh is not authed, note it in the report and continue)
   - **Every master promotion is THREE-PLATFORM, and the platform
     suites GATE it** (owner rule, 2026-08-20). Assets upload only
     after ALL THREE lanes are built, verified, and green — a Windows
     or macOS regression BLOCKS the promotion and gets fixed first,
     never recorded as a residue.
     1. Linux: `bash scripts/package_release.sh` on the build container
        (rebuilds in the distribution configuration, runs the packed
        suite against the exact packaged binary, restores the tree;
        TRUNCATES `dist/SHA256SUMS`).
     2. Windows: `scripts/remote_build.sh release-win` (build +
        verify_pe_release), then the wine packed suite
        (`WINEDEBUG=-all WINEPATH='Z:\workspace\madc\bin'
        MADC_BIN=bin/madc-release-x86-64-windows.exe MADC_WRAPPER=wine
        MADC_SKIP_EXT='win64 wine64' bash scripts/run_tests.sh`) —
        must be green — then `bash scripts/package_release_windows.sh`
        (appends to SHA256SUMS).
     3. macOS: `scripts/remote_build.sh release-macos` (both arches +
        verify_macho_release), then
        `bash scripts/package_release_macos.sh` (appends to SHA256SUMS);
        run the Mac battery (ssh derek@192.168.1.79, `LC_ALL=C`) when
        the Mac is reachable — a darwin regression blocks too.
     Pull `dist/` back via scp (NEVER `remote_build.sh sync` before the
     pull — sync clobbers container `dist/`), then upload the SIX
     assets:
     `gh release upload vX.Y.Z dist/madc_*.deb dist/madc-*.rpm
     dist/madc-*-windows-x86_64.zip dist/madc-*-macos-arm64.tar.gz
     dist/madc-*-macos-x86_64.tar.gz dist/SHA256SUMS`

   (MIR needs nothing separate: it lives in-tree at `third_party/mir`,
   so the madc tag versions it — subtree migration 2026-08-11.)

6. **Switch back to develop**

7. **Report**: Print confirmation with version number, tag, and GitHub URL
