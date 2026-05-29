# Session Hand-off

- Start each session by reading `AGENTS.md`, `docs/agent-handoff.md`,
  querying `madc-knowledge`, then reading the mirrored repo files:
  `claude_status.json`, `docs/plans/ROADMAP.md`, and the top of `CHANGELOG.md`.
- Form a concrete hypothesis and plan before editing; avoid repeated
  speculative micro-attempts when one reasoned pass is available.
- Treat `madc-knowledge` as authoritative for project memory; treat the
  repo files as synchronized mirrors of that state.
- Update the KG and every affected mirror artifact in the same session
  as the code or docs change.
- Treat reads and writes inside the repo workspace tree as pre-authorized;
  do not stop work to re-negotiate workspace-local file access.
- Do not create ad hoc status files when an existing canonical file
  can be updated instead.
- Query and update `madc-knowledge` when the task changes phases,
  features, gaps, or design decisions.
- If the graph and flat files disagree, verify the live repo state and
  then sync the repo mirrors to the KG.
- End each substantive session with a concise hand-off note: branch,
  working-tree state, validation run, remaining work, updated files.
