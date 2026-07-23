# Agent Handoff

This project is handed back and forth between multiple AI coding tools.
Use this file to keep the hand-off disciplined and to avoid creating
parallel "current status" documents that drift out of sync.

## Canonical Read Order

At the start of a session, read sources in this order:

1. `AGENTS.md` — project briefing, architecture, and rule index.
2. `docs/agent-handoff.md` — this workflow.
3. `madc-knowledge` graph — authoritative project memory for phases,
   features, gaps, rules, and design decisions.
4. `claude_status.json` — mirrored repo snapshot for branch, tests, phase,
   known issues, and near-term focus.
5. `docs/plans/ROADMAP.md` — mirrored actionable backlog and open gaps.
6. `CHANGELOG.md` — mirrored landed-work history.
7. `docs/test-status.md` — detailed test inventory when validation context
   matters.

## Source Of Truth

Use these ownership rules when sources overlap:

| Source | Purpose | Authority |
|--------|---------|-----------|
| `AGENTS.md` | Rules, architecture, process | Canonical for agent behavior |
| `madc-knowledge` KG | Phases, features, gaps, rules, decisions | Canonical project-memory source |
| `claude_status.json` | Current snapshot | Mirror of KG + live repo validation state |
| `docs/plans/ROADMAP.md` | Actionable backlog | Mirror of KG open work and priorities |
| `CHANGELOG.md` | Historical record | Mirror of landed work recorded from KG / repo changes |
| `docs/test-status.md` | Detailed test coverage | Canonical for per-test baseline |

If sources disagree, resolve conflicts in this order:

1. Live repo state: current branch, working tree, and actual test/build output.
2. `AGENTS.md` for process and rule behavior.
3. `madc-knowledge` for project memory.
4. `claude_status.json`, `docs/plans/ROADMAP.md`, and `CHANGELOG.md` as repo mirrors.

Do not create new ad hoc status files when an existing source can be
updated instead.

Workspace-tree file access is presumed approved. Do not interrupt the user
for ordinary reads or writes under the repo root; only surface permission
friction for out-of-workspace, network, or destructive operations.

## Session Start Checklist

- Confirm the live branch and working-tree status.
- Query `madc-knowledge` first for the current phase, open gaps, recent
  decisions, and any relevant rule nodes.
- Read `claude_status.json`, `docs/plans/ROADMAP.md`, and `CHANGELOG.md` as mirrors of that
  state and as repo-local working summaries.
- Decide early whether the task is core-only or `madcdat`-affecting.
  For core-only work, prefer a workspace configured with
  `./configure --enable-madcdat=no`; for storage/shared-surface work,
  re-enable `madcdat` before final validation.

## Default Work Split

Use this split unless the current task clearly wants a different one:

- Collaboration rule
  - no agent is the boss
  - both agents should converge on the best available plan
  - defer to evidence, not tool identity
- Shared expectation
  - inspect first
  - form a hypothesis before editing
  - prefer one coherent pass over many speculative retries
- Codex CLI
  - bounded implementation work
  - test creation and regression coverage
  - repo-wide status reconciliation
  - mechanical follow-through after the design is already clear
- Claude Code
  - exploratory design and compiler-path reasoning
  - longer-form synthesis across rules, docs, and prior work
  - review-style passes over open gaps and architectural tradeoffs
  - shaping the next task when the path is still ambiguous

Either agent can do any task. This split is a default for reducing
duplicate exploration, not a hard capability boundary.

## Branch Ownership

- Keep `develop` as the shared integration branch.
- Put active unmerged work on agent-owned feature branches:
  - `feature/<topic>-claude`
  - `feature/<topic>-codex`
- Avoid having both agents edit the same feature branch concurrently.
- When handing a feature branch from one agent to the other, say so in the
  final hand-off note and update `claude_status.json` if the active branch
  changed.
- Use separate agent-owned branches when both agents are exploring the same
  general area but different implementations or different subproblems.

## Session End Checklist

After a substantive change, update all applicable artifacts before
handing off:

- `claude_status.json`
  - Sync it from the updated KG plus live repo state.
  - Update `date`.
  - Update `branch` if the live branch changed.
  - Update build/test counts if they changed or were revalidated.
  - Add newly completed features, open issues, or current focus.
- `docs/plans/ROADMAP.md`
  - Sync it from the updated KG gaps / open work.
  - Remove completed items.
  - Add newly discovered gaps or regressions.
  - Keep priority placement honest.
- `CHANGELOG.md`
  - Mirror landed work recorded in the KG.
  - Append landed work under `[Unreleased]`.
  - Record behavior changes and new tests, not scratch notes.
- `docs/test-status.md`
  - Update when tests are added, removed, renamed, or materially re-scoped.
- `madc-knowledge`
  - Update this first for any changed `Feature`, `Gap`, `Decision`, `Phase`,
    or `Rule` state.

If a session only explored or reviewed without changing repo state, leave
the files alone and say so in the hand-off note.

## KG Usage

The KG is useful for:

- phase tracking
- feature / gap lookup
- durable design decisions
- remembering work that spans multiple sessions

The KG is authoritative for project memory, but it is not sufficient by
itself for:

- current branch
- working-tree cleanliness
- exact test counts unless explicitly refreshed
- whether a repo file was updated this session

When the KG and repo disagree, verify the live repo state, update the KG,
then sync the repo mirrors.

## Hand-off Note Template

Use this short structure in the final message when handing off between
agents:

- branch and working-tree state
- what changed
- what was validated
- what remains open
- which files and KG nodes were updated

Keep the note short. The durable detail belongs in the repo files above.
