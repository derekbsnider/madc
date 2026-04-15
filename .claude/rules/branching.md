# Branching Model

- Use feature branches off `develop` for new features: `feature/feature-name`
- Keep `develop` stable — all tests must pass before merging
- Merge feature branches into `develop` via PR when stable
- Merge `develop` into `master` for releases
- Commit early and often on feature branches
- Use `#ifdef FEATURE_NAME` guards for in-progress code on shared branches
