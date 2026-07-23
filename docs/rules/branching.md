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

## Why the promote gate is "all class-(a) green", not "match asmjit's 1645"

The original gate said develop must match or exceed master's asmjit-backend
torture count (1645/1685). The 2026-06-11 failset classification audit
(`docs/parity/failset-classification.md`, user-signed) showed that number no
longer measures the right thing:

- The two backends' capability sets diverged — CIR passes 34 tests asmjit
  fails, asmjit passes 82 CIR fails — so comparing raw counts compares
  different feature sets, not progress toward correctness.
- Classifying every remaining failure found 33 tests that exist only to
  exercise gcc internals, gcc-only extensions with no real-world use, or
  undefined behavior where madc's choice is as defensible as gcc's (clang
  diverges from gcc on several of them too). Chasing those is parity
  theater, and the user ruled 100% torture parity is explicitly not the
  goal.
- What actually gates promotion is standard-C correctness: the 41 class-(a)
  failures are real compliance bugs (dominated by K&R old-style definitions
  and implicit-decl handling — exactly the C89 SMAUG-class material madc
  exists for). Hence: gate = all class-(a) fixed = ≥1608 of the 1652
  in-scope tests, class-(c) formally skipped, class-(b) on the roadmap.
