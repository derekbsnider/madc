# Feature Guards

- Wrap new/untested features in `#ifdef FEATURE_NAME` / `#endif` guards
- Define feature macros in a central location or pass via `CXXFLAGS=-DFEATURE_NAME`
- This allows quick enable/disable without reverting code
- Remove guards once the feature is stable and committed
- Never use `git checkout` on files with uncommitted work — use guards or `git stash` instead
- Commit working code before starting new features
- When bisecting bugs, disable features with `#ifdef` — don't delete code
