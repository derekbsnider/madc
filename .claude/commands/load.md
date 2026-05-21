# /load — Bootstrap a fresh session with full project context

Read all key project files to understand current state, then report a summary.

## Steps

1. **Read core status files** (in parallel where possible):
   - `claude_status.json` — primary project state (version, phases, features, known issues, TODOs)
   - `VERSION` — current semantic version
   - `README.md` — project overview, current release, roadmap
   - `CHANGELOG.md` — recent changes (read the first `[Unreleased]` and most recent versioned section)
   - `TODO.md` — remaining work items by priority
   - `docs/SMAUG_requirements.md` — gap analysis for the SMAUG 1.8 goal

2. **Scan the docs/ directory tree**:
   - List all files in `docs/` recursively
   - Note which documentation areas exist (language features, rules, release notes, architecture)

3. **Check git state**:
   - Current branch
   - Any uncommitted changes
   - Commits ahead/behind of origin
   - Last 5 commit messages on current branch

4. **Suggest running tests** (do NOT run them automatically):
   - Mention that the user can run `make -C src fulltest` to verify build + test status before starting work
   - Only run tests if the user explicitly asks or passes `--test` / `--fulltest`

5. **Read project rules**:
   - List all files in `.claude/rules/`
   - Read each rule file (they're short)

6. **Read project commands**:
   - List all files in `.claude/commands/`
   - Note available commands

7. **Report a session briefing** with:
   - Current version and branch
   - What was done in the last release (from CHANGELOG)
   - Top priority items from TODO.md
   - Known issues that might affect current work
   - Available commands (/release, /promote, /save, /load)
   - Any uncommitted work or pending items
