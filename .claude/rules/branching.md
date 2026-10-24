# Branching Model

- Use feature branches off `develop` for new features: `feature/feature-name`
- For agent-owned work-in-progress branches, suffix the branch with the
  active agent name: `-claude` or `-codex`
- Keep one active agent owner per feature branch at a time; hand off the
  branch explicitly before the other agent continues on it
- Keep `develop` stable — all tests must pass before merging
- Merge feature branches into `develop` via PR when stable
- `/release` cuts a versioned release ON `develop` — fine for milestones
- Pushing `develop` requires the lane-freshness gate green:
  `scripts/lane_ledger.sh check --promote` over `docs/lane-status.tsv` —
  every push-gated lane (`promote=yes`) ran green on code-equivalent
  content. The tracked pre-push hook (`scripts/git-hooks/pre-push`, installed
  via `git config core.hooksPath scripts/git-hooks`) enforces it; record lane
  greens with `scripts/lane_ledger.sh record <lane> <tally>`. Feature
  branches push freely. Emergency bypass `MADC_PUSH_NOGATE=1` is loud.
- OWNER LAW (2026-09-04): no master release until EVERY platform lane has run
  its FULL test suite green on the promoted content. Pushing `master` (and
  `/promote`) runs `scripts/lane_ledger.sh check --release`: the develop set
  PLUS the release tier (`promote=release`) — the darwin full suite on both
  mac runner arches (THE libc++ lane: the macOS -O2 packed binary), genuine
  Windows. One stdlib flavor per platform, one per packed binary: Linux and
  Windows libstdc++, macOS libc++ (owner 2026-10-03). A
  build-and-verify lane (the container cross `macos` build) is not a test
  lane. A platform failure is fixed or formally skipped with a stated reason
  before promotion — never carried as a residue.
- Do NOT promote `develop` → `master` (`/promote`) until the gcc-torture
  promote gate is met: ALL class-(a) standard-C failures fixed (≥1608 of the
  1652 in-scope tests), with the 33 class-(c) tests formally skipped per
  `docs/parity/failset-classification.md`. Class-(b) GNU extensions are
  roadmap items, not gate blockers. SMAUG running is a milestone, NOT parity.
  See `docs/adr/0001-cir-c2mir-backend.md`.
- Commit early and often on feature branches
- Use `#ifdef FEATURE_NAME` guards for in-progress code on shared branches
- `/dupaudit` runs inside `/commit`, scoped to the commit's diff — NEVER as a
  merge gate; a family it reports as divergent is a live bug, not debt
- Every duplication a `/dupaudit` finding gets consolidated leaves a gate behind
  in `fulltest` — one implementation without a gate regrows
- OWNER LAW (2026-10-16): every release REDUCES existing duplication, not only
  stops new copies — the arc consolidates at least one EXISTING family found by
  a whole-subsystem `/dupaudit` sweep (its own commits, Tier 1 + Tier 2, before
  the seam battery), leaves its gate, and the release's CHANGELOG section names
  it: `Consolidated: <DupFamily> — gate scripts/check-<name>.sh`. Gated by
  `scripts/check-release-consolidation.sh` (`/release` step 5). A PATCH
  release (x.y.Z, Z > 0: regression fixes on a release) is exempt (owner
  2026-10-24)
- OWNER LAW (2026-10-16): every release moves C23 and/or C++20 conformance
  forward — it raises at least one passing count over the previous release and
  lowers none, on the same in-scope set (`docs/parity/conformance-ledger.tsv`:
  c23-compile, c23-run, torture-c23, cpp2a). The counts are written only by
  `scripts/conformance_measure.sh --record` — the previous release on its
  archived binary, the candidate on the battery's madc-release — and the
  release's CHANGELOG section names its gain:
  `Conformance: <corpus> <old> -> <new>`. Gated by
  `scripts/check-release-conformance.sh` (`/release` step 5). A PATCH release
  is measured and lowers none; it needs no rise and no line (owner 2026-10-24)
- Every release since v0.102.1 carries its callgrind map
  (`scripts/release_bins.sh map vX.Y.Z`, `/release` step 13); the master push
  gate's `release_bins.sh check` refuses a release without one (owner
  2026-10-08)
