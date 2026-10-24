# /release-candidate — validate, package and ship a release, one fixed order

Owner, 2026-10-24: "we need more scripts, and maybe commands/skills to ensure
certain procedures follow the same process". Before this command, each release
re-wrote its procedure as scratch scripts. v0.106.0 ran three days late and
still needed v0.106.1, because what those scratch scripts left out showed up
after the tag. Every step below is a TRACKED script. Do not hand-drive one or
write a replacement in tmp/. A step that is wrong gets fixed in its script.

Where each runs: **C** = the build container (`ssh -p 2299 dev@localhost`), one
heavy job at a time; **N** = the NAS (the trees, the ledger, gh, the ssh keys;
it builds and runs nothing); **G** = GitHub's runners.

## Order

0. **N — sync**: `bash scripts/remote_build.sh sync`, then the post-sync steps
   on C: `cp -p tmp/runprobe/hello.cpp hello.cpp`, and move
   `tests/testnoreturn_c11.*` to `tmp/held_noreturn/`. Never sync while a C
   job reads the tree.
1. **C — the previous release first**:
   `bash scripts/release_candidate.sh diff` runs `release_diff.sh`: startup,
   exec with the placement shift test, compile, memory, callgrind cost, and
   tests/ with new stderr. A RED row is fixed before ANY long suite, or
   excused for this version in `docs/parity/release-diff-allow.tsv` with its
   reason. Record `release-diff` (N).
2. **N — the release content**: `/release` (VERSION, CHANGELOG, notes,
   README, claude_status, the "Release vX.Y.Z — …" commit). Push develop.
3. **N → G — rehearse**: `bash scripts/release_rehearsal.sh`. It runs
   release.yml on the candidate into the draft, then madcide's CI on every
   platform against that draft, and records `release-ci` and `products`.
   Start it now: it runs on G, in parallel with C.
4. **C — commit tier**: `bash scripts/release_candidate.sh fast` (records
   itself on C; mirror the rows to N).
5. **N — the seam battery**: `bash scripts/seam_battery.sh` FROM THE NAS.
   Record `linux-battery`, `headerless-win`, `macos`, `aarch64-ld` and
   `tests-jit` from its log.
6. **C — platform lanes**: `bash scripts/release_candidate.sh lanes < /dev/null`
   runs wine64, interactive, genuine-win, interactive-win and brew-linux.
   Record each (N).
7. **N — the Macs**: `bash scripts/mac_release.sh lanes arm64`, then
   `lanes x86-64`. Record `darwin-suite` and `interactive-darwin` with both
   arches' tallies.
8. **C — conformance**: `bash scripts/release_candidate.sh conformance`. Copy
   `docs/parity/conformance-ledger.tsv` back to N.
   `check-release-conformance.sh` and `check-release-consolidation.sh` must
   pass (`/release` step 5).
9. **C — packages**: `bash scripts/release_candidate.sh packages` builds
   madc's set into `tmp/v<V>-artifacts/`, plus the release archive, the
   callgrind map and `release_bins.sh check`.
10. **Products**:
    - N: `bash scripts/mac_release.sh products-sync`.
    - C: `bash scripts/release_candidate.sh products` (madcide + Chthonia on
      Linux, wine and 22.04).
    - N: `bash scripts/mac_release.sh products arm64`, then `products x86-64`.
11. **C — the packages against the previous release's**:
    `bash scripts/release_diff.sh packages --prev-assets=<the previous release's
    assets, gh release download on N> --cand-assets=tmp/v<V>-artifacts`.
12. **N — stage**: `bash scripts/release_stage.sh` puts madcide's and
    Chthonia's assets under GitHub's names, sums checked.
13. **The owner tests the artifacts on all three environments**, then gives
    the explicit go. Never promote or publish without it.
14. **N — promote and publish**: `bash scripts/promote_release.sh promote`.
    Then, after release.yml is green on the tag, run
    `bash scripts/promote_release.sh publish`. Then madcide, then Chthonia:
    push main and the tag, then `gh release create` with the staged assets and
    the notes. Chthonia's commits carry no AI attribution.

`lane_ledger.sh check --release` must show every row FRESH before step 14;
the master pre-push hook refuses otherwise.
