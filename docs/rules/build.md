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
library (libmir + c2mir) at `/workspace/mir`, and the old asmjit link
flags no longer exist.
