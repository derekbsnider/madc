# Branching Model

- Use feature branches off `develop` for new features: `feature/feature-name`
- For agent-owned work-in-progress branches, suffix the branch with the
  active agent name: `-claude` or `-codex`
- Keep one active agent owner per feature branch at a time; hand off the
  branch explicitly before the other agent continues on it
- Keep `develop` stable — all tests must pass before merging
- Merge feature branches into `develop` via PR when stable
- `/release` cuts a versioned release ON `develop` — fine for milestones
- Pushing `develop` or `master` requires the lane-freshness gate green:
  `scripts/lane_ledger.sh check --promote` over `docs/lane-status.tsv` —
  every push-gated lane ran green on code-equivalent content. The tracked
  pre-push hook (`scripts/git-hooks/pre-push`, installed via
  `git config core.hooksPath scripts/git-hooks`) enforces it; record lane
  greens with `scripts/lane_ledger.sh record <lane> <tally>`. Feature
  branches push freely. Emergency bypass `MADC_PUSH_NOGATE=1` is loud.
- Do NOT promote `develop` → `master` (`/promote`) until the gcc-torture
  promote gate is met: ALL class-(a) standard-C failures fixed (≥1608 of the
  1652 in-scope tests), with the 33 class-(c) tests formally skipped per
  `docs/parity/failset-classification.md`. Class-(b) GNU extensions are
  roadmap items, not gate blockers. SMAUG running is a milestone, NOT parity.
  See `docs/adr/0001-cir-c2mir-backend.md`.
- Commit early and often on feature branches
- Use `#ifdef FEATURE_NAME` guards for in-progress code on shared branches
- Before merging a feature branch, run `/dupaudit` scoped to the subsystem the
  feature touched; a family it reports as divergent is a live bug, not debt
- Every duplication a `/dupaudit` finding gets consolidated leaves a gate behind
  in `fulltest` — one implementation without a gate regrows
