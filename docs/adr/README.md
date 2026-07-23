# Architecture Decision Records (ADRs)

Each ADR captures one significant architectural decision: its **context**, the
**decision**, the **alternatives** considered, and the **consequences**
(what's gained and what's accepted as a cost). One file per decision,
`NNNN-short-title.md`, numbered in order.

ADRs are **append-only**. A decision is never edited away — when a later
decision overrides it, the old ADR's status becomes `Superseded by NNNN` and a
new ADR is added. This keeps a durable trail of *why* the architecture is the
way it is.

Mirror each ADR's gist into a KG `Decision` node (per
`.claude/rules/knowledge-graph.md`) so the graph and the repo agree.

| ADR | Title | Status |
|-----|-------|--------|
| [0001](0001-cir-c2mir-backend.md) | The CIR / c2mir backend (a C-AST IR feeding MIR), not a direct-MIR retarget | Accepted |
