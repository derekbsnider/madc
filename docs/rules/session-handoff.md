# Session Hand-off — Why

madc is now being passed between multiple AI coding agents that all have
repo access, rule access, and knowledge-graph access. Without an explicit
hand-off rule, each session tends to invent its own scratch summary or to
trust a stale file, which creates two bad outcomes:

1. the next agent starts by re-discovering context instead of moving work
   forward
2. the repo accumulates overlapping "current status" files that disagree

The hand-off rule exists to force edits back into the durable sources that
already matter:

- `madc-knowledge` as the authoritative project-memory source
- `claude_status.json` for the current snapshot
- `docs/plans/ROADMAP.md` for open work
- `CHANGELOG.md` for landed work
- `docs/test-status.md` for test coverage detail

The repo files are still important, but they now mirror the KG rather than
defining the durable project memory themselves. The practical exceptions are
live git state and actual validation output: branch names, working-tree
cleanliness, and exact build/test results are still verified from the repo
before syncing the KG and the mirrored files.

The session-end note stays intentionally short. If the note becomes a second
changelog or a second TODO list, the hand-off system has failed. Durable
detail belongs in the canonical files, not in chat history.

The same applies to execution style. In a shared-token environment, rapid
"try something, fail, try something else" loops are expensive. The preferred
pattern is: inspect, form a hypothesis, make the smallest coherent change,
then validate. Iteration still happens, but it should be evidence-driven
rather than speculative.

The same principle applies to permissions inside the repo. Workspace-local
file reads and writes should be treated as pre-authorized. If the runtime
needs an internal escalated flag to satisfy the shell wrapper, that should
happen without turning into a conversational blocker. User interruption is
for materially different actions: network access, out-of-workspace paths, or
destructive commands.

This rule also makes task division possible. When both agents follow the
same read order and update contract, work can be split across sessions or
branches without losing state in transit.
