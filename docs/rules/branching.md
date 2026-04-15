# Branching Model — Why

Feature branches isolate new work from the stable `develop` branch. This was adopted after discovering that uncommitted changes across multiple files are fragile — a single bad `git checkout` can destroy everything.

The workflow: `feature/name` branch off `develop`, commit early and often, merge back via `--no-ff` when stable. This keeps the git history clean and makes it easy to compare feature vs baseline.

`develop` is the integration branch — all tests must pass. `master` is for releases only.
