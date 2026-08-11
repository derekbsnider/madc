# ADR 0002 — MIR lives in-tree: `third_party/mir` as a Git subtree

- **Status:** accepted, executed 2026-08-11
- **Owner decision:** 2026-08-11 ("let's do it"), plan drafted by the
  owner the same day
- **Plan / execution record:**
  [docs/plans/mir-into-madc-repo-2026-08-11.md](../plans/mir-into-madc-repo-2026-08-11.md)
- **Supersedes:** the standalone-fork model (`MIR_COMMIT` pin,
  `MIR_VERSION` release pairing, mirrored `develop`/`master` branches,
  lockstep merge/tag/release ceremony)

## Decision

The MADC-specific MIR/c2mir downstream moved from the separately
maintained `derekbsnider/mir` repository into this repository at
`third_party/mir`, imported as a **Git subtree with preserved history**
(no squash) at the exact commit the final `MIR_COMMIT` pinned
(`fde19e17`, also released as fork `v1.0-madc.0.76.0` and tagged
`madc-pre-subtree-migration`). Tree-hash equality against the pin was
proven at import.

`third_party/mir` is **ordinary madc source**: a MIR-touching fix and
the madc feature that needs it land in one commit, on one branch, in
one review. No pin, no separate push, no branch correspondence, no
separate MIR release — the madc commit IS the pin and the madc release
IS the MIR release.

## Repository roles

| Repository | Role |
|---|---|
| `derekbsnider/madc` (`third_party/mir`) | the only canonical, maintained MADC MIR downstream |
| `vnmakarov/mir` | true upstream — reviewed, deliberately synced, PR target |
| `derekbsnider/mir` | historical former downstream + transport for clean upstream PR branches; its `master`/`develop` are frozen, never synced with the subtree |

## Policy

- MADC-specific MIR changes live only in MADC.
- Generic MIR/c2mir bugs are fixed in-tree AND exported for upstream
  when appropriate: `git subtree split` → cherry-pick onto
  `vnmakarov/mir master` in a temp worktree → test against stock MIR →
  push through `derekbsnider/mir` → PR (plan §17).
- Incoming upstream changes flow directly `vnmakarov/mir` →
  `third_party/mir` via deliberate, reviewed subtree pulls (no
  automation; plan §19–§20).
- Subtree mechanics are boundary operations only — normal development
  never runs them.
- libmir/c2m build products land under `obj/mir/<variant>`, never
  inside the subtree (`make -C src` builds them; `mirclean` removes
  them). A build must never dirty `third_party/mir`.

## Consequences

- A fresh `git clone` + `./configure` + `make -C src` builds everything
  — no second repository, no pin checkout (the historical friction this
  ADR removes: contributors previously had to clone the fork's exact
  pinned commit by hand).
- `MIR_COMMIT`, `MIR_VERSION`, the fork release scheme, and the
  `/release`–`/promote` fork-lockstep steps are retired (reasoning
  preserved in `docs/rules/build.md`, labeled historical).
- madc's Git history now carries MIR's ancestry (~10 years); accepted
  for provenance, upstream comparison, and patch extraction.
- In-flight upstream PRs (mir#461/#462/#463) continue riding their
  branches on `derekbsnider/mir` until resolved.
