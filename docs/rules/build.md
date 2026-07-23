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

## Fork releases and version pairing — why

The fork's release version is `<upstream-base>-madc.<madc-version>`
(owner decision, 2026-07-23, superseding the 2026-07-22 madc-vX.Y.Z
naming): derived-project practice keeps the original project's version
visible with the derivation after the dash, so `1.0-madc.0.38.0` says at
a glance "upstream MIR 1.0, carrying the divergence madc 0.38.0 ships
against." The upstream base answers "how stale are we against upstream?"
(it bumps only when a newer upstream release is merged in); the suffix
answers "which madc consumes this?" The full madc triplet is used (not
`0.38`) so a patch release that touches the fork gets its own fork
release. The `madc.` infix keeps our tags unmistakably separate from
upstream's own `vX.Y.Z` tags, which share the fork repo's namespace.

Three mechanisms carry the dependency, each doing one job:

- **`MIR_COMMIT`** is the machine pin: exact, per-commit, what the build
  actually checks out. It moves whenever madc starts depending on new
  fork work, which can be many times between releases.
- **`MIR_VERSION`** is madc's declared dependency on a fork RELEASE —
  the human-readable "this madc requires fork 1.0-madc.0.38.0". It moves
  only at madc releases that ship fork changes; between releases it
  names the newest published fork release (the pin may be ahead of it).
- **The annotated `v<MIR_VERSION>` tag** on the fork is that release:
  placed on the commit `MIR_COMMIT` pins at madc-release time, pushed
  with the release. A madc release with an unchanged fork cuts no new
  fork release — the previous fork release simply gains another
  dependent, which is why the fork version is not blindly mirrored from
  madc's.

Restating the pin's VALUE in prose (rules files, AGENTS.md) proved to be
a drift trap — the literal `2ffebff` sat stale in two documents while
the real pin had moved twice. Hence the rule: the file is the single
source of truth; documents reference the file, never the value.
