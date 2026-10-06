# Session Hand-off

- Start each session by reading `AGENTS.md`, `docs/agent-handoff.md`,
  querying `madc-knowledge`, then reading the mirrored repo files:
  `claude_status.json`, `docs/plans/ROADMAP.md`, and the top of `CHANGELOG.md`.
- Before editing, establish the current task, expected outcome, relevant
  constraints, and implementation plan. Prefer a deliberate implementation
  pass over repeated speculative micro-changes.
- Treat `madc-knowledge` as authoritative for project memory; treat the
  repo files as synchronized mirrors of that state.
- Update the KG and every affected mirror artifact in the same session
  as the corresponding code or documentation change.
- Treat reads and writes inside the repo workspace tree as pre-authorized;
  do not stop work to re-negotiate workspace-local file access.
- Do not create ad hoc status files when an existing canonical file
  can be updated instead.
- Query and update `madc-knowledge` when the task changes phases,
  features, gaps, or design decisions.
- If the graph and flat files disagree, verify the live repo state and
  then synchronize the repo mirrors with the KG.
- End each substantive session with a concise hand-off note containing
  only operational state: branch, working-tree state, validation performed,
  remaining work, and files updated.
