# Branching Model

- Use feature branches off `develop` for new features: `feature/feature-name`
- For agent-owned work-in-progress branches, suffix the branch with the
  active agent name: `-claude` or `-codex`
- Keep one active agent owner per feature branch at a time; hand off the
  branch explicitly before the other agent continues on it
- Keep `develop` stable — all tests must pass before merging
- Merge feature branches into `develop` via PR when stable
- `/release` cuts a versioned release ON `develop` — fine for milestones
- Do NOT promote `develop` → `master` (`/promote`) until the gcc-torture
  promote gate is met: ALL class-(a) standard-C failures fixed (≥1608 of the
  1652 in-scope tests), with the 33 class-(c) tests formally skipped per
  `docs/parity/failset-classification.md`. Class-(b) GNU extensions are
  roadmap items, not gate blockers. SMAUG running is a milestone, NOT parity.
  See `docs/adr/0001-cir-c2mir-backend.md`.
- Commit early and often on feature branches
- Use `#ifdef FEATURE_NAME` guards for in-progress code on shared branches
