# Build — Why

madc now has two materially different validation footprints:

1. core compiler / runtime / `libmadc`
2. optional `madcdat` storage/federation backends and their unit tests

When the task is clearly in the first bucket, leaving `madcdat` enabled
just burns time:

- more object files rebuild
- more unit binaries relink
- more optional backend dependencies stay in the path

That extra coverage is valuable when a change may affect shared public
headers, build wiring, or the storage layer itself. It is mostly noise
when the task is isolated to parser/codegen/runtime/public-embedding work.

So the operational rule is:

- for core-only work, prefer `./configure --enable-madcdat=no`
- for storage work, or anything that might affect storage, re-enable it
  before final validation

This is a workflow default, not a license to skip relevant coverage. If a
change can plausibly affect `madcdat`, the broader enabled build remains
the honest gate.

## Backend — why no asmjit flags

The build used to link the manually installed asmjit v1.14 at
`/usr/local/`, with `-L/usr/local/lib -Wl,-rpath,/usr/local/lib` in the
Makefile to avoid the older apt-packaged copy. asmjit (the original
x86-64 JIT) was removed: madc now lowers its AST to a `cir_node` C11 IR
that c2mir compiles to MIR. The backend dependency is therefore the MIR
library (libmir + c2mir) at `/workspace/mir`, on the madc fork's `develop`
branch and pinned by the repo-root `MIR_COMMIT` file. The old asmjit link flags
no longer exist.

## Release tag pairing — why

The fork has no version identity of its own — it exists to serve madc,
so its versioning is madc's (owner decision, 2026-07-22). Two mechanisms
carry that pairing, each doing one job:

- **`MIR_COMMIT`** is the machine pin: exact, per-commit, what the build
  actually checks out. It moves whenever madc starts depending on new
  fork work, which can be many times between releases.
- **The annotated `madc-vX.Y.Z` tag** on the fork is the human-readable
  release correspondence: "this fork state is what madc vX.Y.Z shipped
  against." One tag per madc release, placed on the commit `MIR_COMMIT`
  pins at release time, pushed alongside the release.

Restating the pin's VALUE in prose (rules files, AGENTS.md) proved to be
a drift trap — the literal `2ffebff` sat stale in two documents while
the real pin had moved twice. Hence the rule: the file is the single
source of truth; documents reference the file, never the value.
