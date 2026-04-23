# Branching Model — Why

Feature branches isolate new work from the stable `develop` branch. This was adopted after discovering that uncommitted changes across multiple files are fragile — a single bad `git checkout` can destroy everything.

The workflow: `feature/name` branch off `develop`, commit early and often, merge back via `--no-ff` when stable. This keeps the git history clean and makes it easy to compare feature vs baseline.

`develop` is the integration branch — all tests must pass. `master` is for releases only.

Now that work is handed back and forth between Claude Code and Codex CLI,
agent-owned feature branches reduce branch collisions and make unfinished work
safe to park. Suffixing branches with `-claude` or `-codex` makes ownership
obvious in `git branch` output and in hand-off notes:

- `feature/hashstr-next-claude`
- `feature/hashstr-next-codex`

The point is not permanent silos. It is to make WIP explicit. When one agent
hands a branch to the other, that hand-off should be recorded in the session
note and in the updated status files so both tools agree about who owns the
next edit.
